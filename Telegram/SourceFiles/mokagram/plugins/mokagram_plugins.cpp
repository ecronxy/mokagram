// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: c69c989a
// Mokagram: хост плагинов (встроенный Python).
#if defined(__linux__)
// Linux: Python живёт в дочернем процессе под bwrap, в клиент он не встраивается.
#define MOKA_PLUGINS_PROCESS 1
#else
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif
#include "mokagram/lang/mokagram_lang.h"

#ifdef _WIN32
#include <windows.h>
#include <cwchar>
#endif

#include "mokagram/plugins/mokagram_plugins.h"

#ifdef MOKA_PLUGINS_PROCESS
#include <QtCore/QByteArray>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <climits>
#include <filesystem>
#include <cstring>
#include <optional>
extern char **environ;
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Mokagram::Plugins {
namespace {

// Исходники лежат в py/*.py, .inc делает py/gen_inc.sh.
#include "mokagram/plugins/py/tl_names.inc"
#include "mokagram/plugins/py/host.inc"
#include "mokagram/plugins/py/base_plugin.inc"
#include "mokagram/plugins/py/sdk_api.inc"
#include "mokagram/plugins/py/tl.inc"
#include "mokagram/plugins/py/fw.inc"
#ifdef MOKA_PLUGINS_PROCESS
#include "mokagram/plugins/py/proc_main.inc"
#endif
#include "mokagram/plugins/py/tl_schema.inc"

constexpr auto kQueueLimit = size_t(64);
constexpr auto kMaxHookBytes = size_t(256 * 1024);
// Потолок памяти очереди: 64 * 256 КБ = 16 МБ. Спамящий канал не должен надувать RSS.
constexpr auto kAuditLimit = std::uintmax_t(5 * 1024 * 1024);
constexpr auto kHookKinds = 4;
constexpr const char *kKindNames[kHookKinds] = {
	"0", "1", "2", "3"
};

std::atomic<bool> _started = false;
bool _helpersLoaded = false;
LogFn _logFn;
BridgeFn _bridge;
std::mutex _bridgeMutex;
std::atomic<bool> _stopping = false;
std::atomic<int> _fromPlugin = 0;

// Подписки плагинов по видам хуков: id плагина -> наборы constructor id.
using IdSet = std::unordered_set<uint32_t>;
std::mutex _filterMutex;
std::unordered_map<std::string, std::array<IdSet, kHookKinds>> _filterByPlugin;
std::array<IdSet, kHookKinds> _filterUnion;
std::atomic<size_t> _filterCount[kHookKinds] = {};

// Очередь наблюдающих хуков и рабочий поток Python.
constexpr auto kEventKind = 100;
struct HookTask {
	int kind = 0;
	uint32_t id = 0;
	std::string bytes;
};
std::mutex _queueMutex;
std::condition_variable _queueCv;
std::deque<HookTask> _queue;
std::thread _worker;
size_t _dropped = 0;

std::mutex _auditMutex;

#ifndef MOKA_PLUGINS_PROCESS
bool EnsurePython(const LogFn &log) {
#ifdef _WIN32
	// pythonNNN.dll подключена отложенно: без неё клиент запускается, просто без плагинов.
	static const bool present = [] {
		wchar_t name[32];
		swprintf(name, 32, L"python%d%d.dll", PY_MAJOR_VERSION, PY_MINOR_VERSION);
		return LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32) != nullptr;
	}();
	if (!present) {
		if (log) log(Mokagram::Lang::Tr("python: нет библиотеки Python рядом с клиентом, плагины отключены"));
		return false;
	}
#endif
	if (!Py_IsInitialized()) {
		Py_InitializeEx(0);
		if (!Py_IsInitialized()) {
			if (log) log(Mokagram::Lang::Tr("python: не удалось инициализировать"));
			return false;
		}
		if (log) log(std::string("python ") + Py_GetVersion());
		// После инициализации поток держит GIL: отдаём его, дальше везде PyGILState_Ensure.
		PyEval_SaveThread();
	}
	return true;
}

// Нужен GIL. Вызывает функцию из __main__ со строковыми аргументами, возвращает строку.
std::string CallStr(const char *function, const std::vector<std::string> &args) {
	auto result = std::string();
	const auto module = PyImport_AddModule("__main__");
	const auto globals = PyModule_GetDict(module);
	const auto func = PyDict_GetItemString(globals, function);
	if (!func) {
		return result;
	}
	const auto tuple = PyTuple_New(static_cast<Py_ssize_t>(args.size()));
	for (auto i = size_t(); i != args.size(); ++i) {
		PyTuple_SetItem(tuple, i, PyUnicode_FromStringAndSize(
			args[i].data(),
			static_cast<Py_ssize_t>(args[i].size())));
	}
	const auto value = PyObject_CallObject(func, tuple);
	Py_DECREF(tuple);
	if (value) {
		if (PyUnicode_Check(value)) {
			if (const auto text = PyUnicode_AsUTF8(value)) {
				result = text;
			}
		} else if (PyLong_Check(value)) {
			result = std::to_string(PyLong_AsLong(value));
		}
		Py_DECREF(value);
	} else {
		PyErr_Clear();
	}
	return result;
}

// Нужен GIL. Переносит накопленный журнал плагинов в LogFn (по строке на запись).
// Безопасно из нескольких потоков: Python отдаёт записи уже одной строкой.
void Drain(const LogFn &log) {
	const auto text = CallStr("_moka_drain", {});
	if (!log || text.empty()) {
		return;
	}
	auto pos = size_t();
	while (pos <= text.size()) {
		const auto end = text.find('\n', pos);
		const auto line = text.substr(
			pos,
			end == std::string::npos ? end : end - pos);
		if (!line.empty()) {
			log("plugin: " + line);
		}
		if (end == std::string::npos) {
			break;
		}
		pos = end + 1;
	}
}
#endif // !MOKA_PLUGINS_PROCESS

std::string NowIso() {
	const auto now = std::time(nullptr);
	auto parts = std::tm();
#ifdef _WIN32
	localtime_s(&parts, &now);
#else
	localtime_r(&now, &parts);
#endif
	char buffer[32];
	std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%S", &parts);
	return buffer;
}

std::string DataRoot() {
	auto path = std::filesystem::path(PluginsDir());
	if (!path.has_filename()) {
		path = path.parent_path();
	}
	return path.parent_path().string();
}

// Журнал действий плагинов: пишет только хост. Ротация 5 МБ x 3 файла.
void AuditWrite(const std::string &json) {
	if (json.size() < 2 || json.front() != '{' || json.size() > 4096) {
		return;
	}
	const auto lock = std::lock_guard(_auditMutex);
	try {
		const auto path = std::filesystem::path(DataRoot()) / "plugin_audit.log";
		std::error_code error;
		std::filesystem::create_directories(path.parent_path(), error);
		if (std::filesystem::exists(path, error)
			&& std::filesystem::file_size(path, error) > kAuditLimit) {
			const auto one = path.string() + ".1";
			const auto two = path.string() + ".2";
			std::filesystem::remove(two, error);
			std::filesystem::rename(one, two, error);
			std::filesystem::rename(path, one, error);
		}
		auto out = std::ofstream(path, std::ios::app);
		out << "{\"ts\":\"" << NowIso() << "\"," << json.substr(1) << '\n';
	} catch (...) {
	}
}

// Текст: первая строка id плагина, дальше «вид id id id». Пустой список снимает подписки.
void SetFilters(const std::string &text) {
	auto stream = std::istringstream(text);
	auto id = std::string();
	if (!std::getline(stream, id) || id.empty()) {
		return;
	}
	auto lists = std::array<IdSet, kHookKinds>();
	auto line = std::string();
	while (std::getline(stream, line)) {
		auto parts = std::istringstream(line);
		auto kind = -1;
		if (!(parts >> kind) || kind < 0 || kind >= kHookKinds) {
			continue;
		}
		auto value = uint64_t();
		while (parts >> value) {
			lists[kind].insert(uint32_t(value));
		}
	}
	const auto lock = std::lock_guard(_filterMutex);
	_filterByPlugin[id] = std::move(lists);
	for (auto kind = 0; kind != kHookKinds; ++kind) {
		_filterUnion[kind].clear();
		for (const auto &[name, sets] : _filterByPlugin) {
			_filterUnion[kind].insert(sets[kind].begin(), sets[kind].end());
		}
		_filterCount[kind].store(
			_filterUnion[kind].size(),
			std::memory_order_relaxed);
	}
}

// Вызов хоста из Python: _moka_native(op, json). Выполняется без GIL.
std::string HandleNative(const std::string &op, const std::string &args) {
	try {
		if (op == "audit") {
			AuditWrite(args);
			return "{}";
		} else if (op == "filters") {
			SetFilters(args);
			return {};
		}
		auto bridge = BridgeFn();
		{
			const auto lock = std::lock_guard(_bridgeMutex);
			bridge = _bridge;
		}
		if (bridge) {
			return bridge(op, args);
		}
	} catch (...) {
	}
	return R"({"ok":false,"error":"клиент недоступен"})";
}

#ifdef MOKA_PLUGINS_PROCESS

// ---- Процесс-хост: Python живёт в дочернем процессе (bwrap), общение по socketpair ----
//
// Кадр: 4 байта длины (big endian), тип (1), id (8), id внешнего вызова (8), число строк (4),
// затем строки (длина 4 + байты). Типы: CALL клиент->хост, RET хост->клиент,
// NATIVE хост->клиент (операция моста), NRET клиент->хост, BOOT клиент->хост (скрипты).
// Запрос NATIVE несёт id вызова CALL, из которого он сделан: его исполняет поток, ждущий этот
// вызов (иначе главный поток, ждущий хост, и хост, ждущий главный поток, повисли бы).
// Запросы без родителя (из потоков самих плагинов) идут в пул потоков.

constexpr auto kFrameCall = uint8_t(1);
constexpr auto kFrameRet = uint8_t(2);
constexpr auto kFrameNative = uint8_t(3);
constexpr auto kFrameNativeRet = uint8_t(4);
constexpr auto kFrameBoot = uint8_t(5);
constexpr auto kFrameLimit = size_t(512) << 20;
constexpr auto kCallTimeout = std::chrono::seconds(15);
constexpr auto kCallHardLimit = std::chrono::seconds(120);
constexpr auto kInitTimeout = std::chrono::seconds(180);
constexpr auto kPoolThreads = 16;
constexpr auto kAutoRestarts = 3;

struct NativeRequest {
	uint64_t id = 0;
	std::string op;
	std::string args;
};

struct Waiter {
	std::condition_variable cv;
	bool done = false;
	bool failed = false;
	std::vector<std::string> value;
	std::deque<NativeRequest> inbox;
};

struct Frame {
	uint8_t kind = 0;
	uint64_t id = 0;
	uint64_t parent = 0;
	std::vector<std::string> strings;
};

struct HostState {
	std::mutex mutex; // waiters, sock, pid
	std::unordered_map<uint64_t, Waiter*> waiters;
	int sock = -1;
	pid_t pid = -1;
	std::atomic<bool> alive = false;
	std::atomic<uint64_t> nextId = 1;
	std::mutex writeMutex;

	std::mutex supMutex;
	std::condition_variable supCv;
	bool supStarted = false;
	bool wantSpawn = false;
	bool firstSpawn = true;
	bool spawnDone = false;
	bool spawnOk = false;
	bool quit = false;
	bool manualRestart = false;
	int autoRestarts = 0;
	std::chrono::steady_clock::time_point spawnedAt;
	bool sandboxed = false;

	std::mutex poolMutex;
	std::condition_variable poolCv;
	std::deque<std::function<void()>> poolQueue;
	int poolThreads = 0;
	int poolIdle = 0;

	std::mutex excludeMutex;
	std::vector<std::string> excluded;
	std::mutex listMutex;
	std::string lastList;
	std::mutex initMutex; // BootAndInit по одному
};

HostState &H() {
	static auto &state = *new HostState;
	return state;
}

void PutU32(std::string &out, uint32_t value) {
	for (auto shift = 24; shift >= 0; shift -= 8) {
		out.push_back(char((value >> shift) & 0xff));
	}
}

void PutU64(std::string &out, uint64_t value) {
	for (auto shift = 56; shift >= 0; shift -= 8) {
		out.push_back(char((value >> shift) & 0xff));
	}
}

uint64_t GetBe(const unsigned char *data, int bytes) {
	auto result = uint64_t();
	for (auto i = 0; i != bytes; ++i) {
		result = (result << 8) | data[i];
	}
	return result;
}

bool SendFrame(
		uint8_t kind,
		uint64_t id,
		uint64_t parent,
		const std::vector<std::string> &strings) {
	auto body = std::string();
	body.push_back(char(kind));
	PutU64(body, id);
	PutU64(body, parent);
	PutU32(body, uint32_t(strings.size()));
	for (const auto &item : strings) {
		PutU32(body, uint32_t(item.size()));
		body += item;
	}
	auto frame = std::string();
	frame.reserve(body.size() + 4);
	PutU32(frame, uint32_t(body.size()));
	frame += body;
	auto &host = H();
	const auto lock = std::lock_guard(host.writeMutex);
	const auto fd = host.sock;
	if (fd < 0) {
		return false;
	}
	auto sent = size_t();
	while (sent != frame.size()) {
		const auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		sent += size_t(n);
	}
	return true;
}

bool ReadExact(int fd, void *data, size_t size) {
	auto done = size_t();
	while (done != size) {
		const auto n = ::recv(fd, static_cast<char*>(data) + done, size - done, 0);
		if (n == 0) {
			return false;
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			return false;
		}
		done += size_t(n);
	}
	return true;
}

bool ReadFrame(int fd, Frame &frame) {
	unsigned char head[4];
	if (!ReadExact(fd, head, 4)) {
		return false;
	}
	const auto size = size_t(GetBe(head, 4));
	if (size < 21 || size > kFrameLimit) {
		return false;
	}
	auto body = std::string(size, '\0');
	if (!ReadExact(fd, body.data(), size)) {
		return false;
	}
	const auto data = reinterpret_cast<const unsigned char*>(body.data());
	frame.kind = data[0];
	frame.id = GetBe(data + 1, 8);
	frame.parent = GetBe(data + 9, 8);
	const auto count = size_t(GetBe(data + 17, 4));
	frame.strings.clear();
	auto pos = size_t(21);
	for (auto i = size_t(); i != count; ++i) {
		if (pos + 4 > size) {
			return false;
		}
		const auto length = size_t(GetBe(data + pos, 4));
		pos += 4;
		if (length > size - pos) {
			return false;
		}
		frame.strings.emplace_back(body.substr(pos, length));
		pos += length;
	}
	return true;
}

void EmitLog(const std::string &text) {
	if (text.empty() || !_logFn) {
		return;
	}
	auto pos = size_t();
	while (pos <= text.size()) {
		const auto end = text.find('\n', pos);
		const auto line = text.substr(
			pos,
			end == std::string::npos ? end : end - pos);
		if (!line.empty()) {
			_logFn("plugin: " + line);
		}
		if (end == std::string::npos) {
			break;
		}
		pos = end + 1;
	}
}

// Пул для запросов хоста без родителя.
void PoolLoop() {
	auto &host = H();
	auto lock = std::unique_lock(host.poolMutex);
	while (true) {
		++host.poolIdle;
		host.poolCv.wait(lock, [&] { return !host.poolQueue.empty(); });
		--host.poolIdle;
		auto job = std::move(host.poolQueue.front());
		host.poolQueue.pop_front();
		lock.unlock();
		try {
			job();
		} catch (...) {
		}
		lock.lock();
	}
}

void PoolPost(std::function<void()> job) {
	auto &host = H();
	const auto lock = std::lock_guard(host.poolMutex);
	host.poolQueue.push_back(std::move(job));
	if (host.poolIdle < int(host.poolQueue.size()) && host.poolThreads < kPoolThreads) {
		++host.poolThreads;
		std::thread(PoolLoop).detach();
	}
	host.poolCv.notify_one();
}

void RunNative(const NativeRequest &request) {
	SendFrame(kFrameNativeRet, request.id, 0, { HandleNative(request.op, request.args) });
}

void FailAllWaiters() {
	auto &host = H();
	const auto lock = std::lock_guard(host.mutex);
	for (const auto &[id, waiter] : host.waiters) {
		waiter->done = true;
		waiter->failed = true;
		waiter->cv.notify_all();
	}
}

void KillHost() {
	auto &host = H();
	auto pid = pid_t(-1);
	{
		const auto lock = std::lock_guard(host.mutex);
		pid = host.pid;
	}
	if (pid > 0) {
		::kill(pid, SIGKILL);
	}
	// Читатель увидит EOF. Если процесс уже мёртв, сокет закроет читатель сам.
	{
		const auto lock = std::lock_guard(host.writeMutex);
		if (host.sock >= 0) {
			::shutdown(host.sock, SHUT_RDWR);
		}
	}
	host.alive = false;
	FailAllWaiters();
}

// Отправляет запрос и ждёт ответ. Пока ждёт, исполняет запросы хоста, вложенные в этот вызов.
std::optional<std::vector<std::string>> Roundtrip(
		uint8_t kind,
		std::vector<std::string> strings,
		std::chrono::seconds timeout) {
	auto &host = H();
	if (!host.alive.load()) {
		return std::nullopt;
	}
	auto waiter = Waiter();
	const auto id = host.nextId.fetch_add(1);
	{
		const auto lock = std::lock_guard(host.mutex);
		host.waiters[id] = &waiter;
	}
	const auto forget = [&] {
		const auto lock = std::lock_guard(host.mutex);
		host.waiters.erase(id);
	};
	if (!SendFrame(kind, id, 0, strings)) {
		forget();
		KillHost();
		return std::nullopt;
	}
	const auto started = std::chrono::steady_clock::now();
	auto deadline = started + timeout;
	auto lock = std::unique_lock(host.mutex);
	while (!waiter.done) {
		if (!waiter.inbox.empty()) {
			const auto request = std::move(waiter.inbox.front());
			waiter.inbox.pop_front();
			lock.unlock();
			RunNative(request);
			lock.lock();
			deadline = std::min(
				std::chrono::steady_clock::now() + timeout,
				started + kCallHardLimit);
			continue;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now >= deadline) {
			break;
		}
		waiter.cv.wait_until(lock, std::min(deadline, now + std::chrono::milliseconds(500)));
	}
	const auto timedOut = !waiter.done;
	lock.unlock();
	forget();
	if (timedOut) {
		if (_logFn) {
			_logFn(Mokagram::Lang::Tr("plugins: хост не ответил вовремя, перезапускаю"));
		}
		KillHost();
		return std::nullopt;
	}
	if (waiter.failed) {
		return std::nullopt;
	}
	return std::move(waiter.value);
}

// Нужен только вызывающему потоку: нетривиальных блокировок нет, возвращает значение функции.
std::string CallStr(const char *function, const std::vector<std::string> &args) {
	auto strings = std::vector<std::string>();
	strings.reserve(args.size() + 1);
	strings.emplace_back(function);
	strings.insert(strings.end(), args.begin(), args.end());
	const auto timeout = (std::string(function) == "_moka_init")
		? kInitTimeout
		: kCallTimeout;
	auto answer = Roundtrip(kFrameCall, std::move(strings), timeout);
	if (!answer || answer->empty()) {
		return {};
	}
	if (answer->size() > 1) {
		EmitLog((*answer)[1]);
	}
	return std::move((*answer)[0]);
}

// Журнал приходит вместе с каждым ответом хоста и сразу уходит в LogFn.
void Drain(const LogFn &) {
}

struct Gil {
	Gil() {
	}
	~Gil() {
	}
};

void ReaderLoop(int fd) {
	auto &host = H();
	auto frame = Frame();
	while (ReadFrame(fd, frame)) {
		if (frame.kind == kFrameRet) {
			const auto lock = std::lock_guard(host.mutex);
			const auto i = host.waiters.find(frame.id);
			if (i != host.waiters.end()) {
				i->second->value = std::move(frame.strings);
				i->second->done = true;
				i->second->cv.notify_all();
			}
		} else if (frame.kind == kFrameNative && frame.strings.size() >= 2) {
			auto request = NativeRequest{
				.id = frame.id,
				.op = std::move(frame.strings[0]),
				.args = std::move(frame.strings[1]),
			};
			auto handed = false;
			if (frame.parent) {
				const auto lock = std::lock_guard(host.mutex);
				const auto i = host.waiters.find(frame.parent);
				if (i != host.waiters.end() && !i->second->done) {
					i->second->inbox.push_back(request);
					i->second->cv.notify_all();
					handed = true;
				}
			}
			if (!handed) {
				PoolPost([request = std::move(request)] {
					RunNative(request);
				});
			}
		}
	}
}

std::string HomeDir() {
	const auto env = std::getenv("HOME");
	return (env && *env) ? env : std::string();
}

std::string SafeRealPath(const std::string &path) {
	char buffer[PATH_MAX];
	return ::realpath(path.c_str(), buffer) ? std::string(buffer) : std::string();
}

bool PathInside(const std::string &base, const std::string &path) {
	return path == base
		|| (path.size() > base.size()
			&& path.compare(0, base.size(), base) == 0
			&& (base.back() == '/' || path[base.size()] == '/'));
}

std::string FindBwrap() {
	for (const auto candidate : { "/usr/bin/bwrap", "/bin/bwrap", "/usr/local/bin/bwrap" }) {
		if (::access(candidate, X_OK) == 0) {
			return candidate;
		}
	}
	if (const auto path = std::getenv("PATH")) {
		auto stream = std::istringstream(path);
		auto part = std::string();
		while (std::getline(stream, part, ':')) {
			const auto full = part + "/bwrap";
			if (!part.empty() && ::access(full.c_str(), X_OK) == 0) {
				return full;
			}
		}
	}
	return {};
}

// Запускает argv, ждёт завершения, возвращает код выхода или -1.
int RunAndWait(const std::vector<std::string> &args) {
	auto argv = std::vector<char*>();
	for (const auto &item : args) {
		argv.push_back(const_cast<char*>(item.c_str()));
	}
	argv.push_back(nullptr);
	auto actions = posix_spawn_file_actions_t();
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 1, "/dev/null", O_WRONLY, 0);
	posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
	auto pid = pid_t();
	const auto code = posix_spawn(&pid, args[0].c_str(), &actions, nullptr, argv.data(), environ);
	posix_spawn_file_actions_destroy(&actions);
	if (code != 0) {
		return -1;
	}
	auto status = 0;
	if (::waitpid(pid, &status, 0) < 0) {
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Есть ли bwrap и работают ли в нём пользовательские пространства имён (ядро и AppArmor бывают против).
std::string UsableBwrap() {
	static const auto path = [] {
		const auto found = FindBwrap();
		if (found.empty()) {
			return std::string();
		}
		const auto code = RunAndWait({
			found, "--unshare-user", "--unshare-pid", "--ro-bind", "/", "/",
			"--dev", "/dev", "/usr/bin/true" });
		return code == 0 ? found : std::string();
	}();
	return path;
}

void Push(std::vector<std::string> &to, std::initializer_list<std::string> items) {
	to.insert(to.end(), items.begin(), items.end());
}

void MakeDirs(const std::string &path) {
	auto error = std::error_code();
	std::filesystem::create_directories(path, error);
}

} // namespace

std::string SharedTempDir() {
	static const auto path = [] {
		auto base = std::string();
		if (const auto env = std::getenv("XDG_RUNTIME_DIR")) {
			struct stat info;
			if (*env == '/' && ::stat(env, &info) == 0 && info.st_uid == ::getuid()) {
				base = env;
			}
		}
		if (base.empty()) {
			base = "/tmp";
		}
		const auto dir = (base == "/tmp")
			? base + "/mokagram-plugins-" + std::to_string(::getuid())
			: base + "/mokagram-plugins";
		if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
			return std::string();
		}
		struct stat info;
		if (::lstat(dir.c_str(), &info) != 0
			|| !S_ISDIR(info.st_mode)
			|| info.st_uid != ::getuid()) {
			return std::string();
		}
		::chmod(dir.c_str(), 0700);
		return SafeRealPath(dir);
	}();
	return path;
}

void SetSandboxExcluded(std::vector<std::string> paths) {
	auto &host = H();
	const auto lock = std::lock_guard(host.excludeMutex);
	host.excluded.clear();
	for (const auto &path : paths) {
		const auto real = SafeRealPath(path);
		if (!real.empty()) {
			host.excluded.push_back(real);
		}
	}
}

namespace {

// Можно ли отдать папку песочнице: внутри домашней, не сама домашняя, не пересекается с запретными.
bool MountAllowed(const std::string &real, const std::string &home) {
	if (real.empty() || home.empty() || real == home || !PathInside(home, real)) {
		return false;
	}
	auto &host = H();
	const auto lock = std::lock_guard(host.excludeMutex);
	for (const auto &banned : host.excluded) {
		if (PathInside(banned, real) || PathInside(real, banned)) {
			return false;
		}
	}
	for (const auto sub : { "/.ssh", "/.config", "/.gnupg", "/.aws", "/.kube" }) {
		const auto secret = home + sub;
		if (PathInside(secret, real) || PathInside(real, secret)) {
			return false;
		}
	}
	return true;
}

std::vector<std::string> SandboxCommand(const std::string &bwrap, const LogFn &log) {
	const auto home = SafeRealPath(HomeDir());
	const auto tmp = SharedTempDir();
	auto args = std::vector<std::string>{
		bwrap,
		"--unshare-user", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
		"--die-with-parent", "--new-session",
		"--clearenv",
		"--ro-bind", "/usr", "/usr",
	};
	for (const auto top : { "/bin", "/sbin", "/lib", "/lib64" }) {
		struct stat info;
		if (::lstat(top, &info) != 0) {
			continue;
		}
		if (S_ISLNK(info.st_mode)) {
			char target[PATH_MAX];
			const auto n = ::readlink(top, target, sizeof(target) - 1);
			if (n > 0) {
				Push(args, { "--symlink", std::string(target, size_t(n)), top });
			}
		} else {
			Push(args, { "--ro-bind", top, top });
		}
	}
	for (const auto etc : {
		"/etc/ssl", "/etc/ca-certificates", "/etc/pki", "/etc/resolv.conf",
		"/etc/hosts", "/etc/nsswitch.conf", "/etc/localtime", "/etc/ld.so.cache",
	}) {
		Push(args, { "--ro-bind-try", etc, etc });
	}
	Push(args, {
		"--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp" });
	const auto dataRoot = DataRoot();
	const auto pluginsDir = PluginsDir();
	const auto bindRw = [&](const std::string &path) {
		MakeDirs(path);
		const auto real = SafeRealPath(path);
		if (MountAllowed(real, home)) {
			Push(args, { "--bind", real, real });
			return true;
		}
		if (log) log("sandbox: папка не монтируется, пропускаю: " + path);
		return false;
	};
	const auto defaultRoot = home + "/.local/share/mokagram";
	if (SafeRealPath(dataRoot) == defaultRoot || (MakeDirs(dataRoot), SafeRealPath(dataRoot) == defaultRoot)) {
		bindRw(dataRoot);
	} else {
		// Нестандартная папка плагинов (разработка): отдаём только её и данные плагинов.
		bindRw(pluginsDir);
		bindRw(dataRoot + "/plugin_data");
	}
	// Журнал аудита хост менять не должен: поверх монтируем его только на чтение.
	{
		const auto audit = SafeRealPath(dataRoot) + "/plugin_audit.log";
		auto touch = std::ofstream(audit, std::ios::app);
		touch.close();
		if (::access(audit.c_str(), F_OK) == 0) {
			Push(args, { "--ro-bind", audit, audit });
		}
	}
	bindRw(tmp);
	for (const auto location : {
		QStandardPaths::DownloadLocation,
		QStandardPaths::PicturesLocation,
		QStandardPaths::MusicLocation,
		QStandardPaths::DocumentsLocation,
	}) {
		const auto path = QStandardPaths::writableLocation(location).toStdString();
		const auto real = SafeRealPath(path);
		if (MountAllowed(real, home)) {
			Push(args, { "--bind", real, real });
		}
	}
	const auto setenv = [&](const std::string &name, const std::string &value) {
		Push(args, { "--setenv", name, value });
	};
	setenv("PATH", "/usr/bin:/bin");
	setenv("HOME", home);
	setenv("TMPDIR", tmp);
	setenv("LANG", "C.UTF-8");
	setenv("PYTHONDONTWRITEBYTECODE", "1");
	setenv("PYTHONUNBUFFERED", "1");
	for (const auto name : {
		"MOKAGRAM_PLUGIN_DEBUG", "MOKAGRAM_DEV_RELOAD", "MOKAGRAM_BACKUP_PEER", "TZ",
	}) {
		if (const auto value = std::getenv(name)) {
			setenv(name, value);
		}
	}
	Push(args, { "--chdir", SafeRealPath(dataRoot) });
	Push(args, { "/usr/bin/python3", "-I", "-c", std::string(kProcMainScript) });
	return args;
}

// Запуск без песочницы (bwrap нет или не работает). Хост всё равно отдельный процесс.
std::vector<std::string> PlainCommand() {
	return { "/usr/bin/env", "python3", "-I", "-c", std::string(kProcMainScript) };
}

bool SpawnHost(const LogFn &log) {
	auto &host = H();
	const auto bwrap = UsableBwrap();
	auto command = std::vector<std::string>();
	if (!bwrap.empty()) {
		command = SandboxCommand(bwrap, log);
		host.sandboxed = true;
	} else {
		command = PlainCommand();
		host.sandboxed = false;
		if (log) log(Mokagram::Lang::Tr("plugins: bwrap не найден или не работает, хост плагинов запущен БЕЗ песочницы (отдельным процессом)"));
	}
	int pair[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
		if (log) log(Mokagram::Lang::Tr("plugins: socketpair не удался"));
		return false;
	}
	// Дочерний конец уводим на дескриптор 3, остальное закрываем в потомке.
	const auto child = ::fcntl(pair[1], F_DUPFD_CLOEXEC, 10);
	if (child < 0) {
		::close(pair[0]);
		::close(pair[1]);
		return false;
	}
	::close(pair[1]);
	auto actions = posix_spawn_file_actions_t();
	posix_spawn_file_actions_init(&actions);
	posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
	posix_spawn_file_actions_adddup2(&actions, child, 3);
	posix_spawn_file_actions_addclosefrom_np(&actions, 4);
	auto argv = std::vector<char*>();
	for (const auto &item : command) {
		argv.push_back(const_cast<char*>(item.c_str()));
	}
	argv.push_back(nullptr);
	auto env = std::vector<std::string>();
	auto envp = std::vector<char*>();
	if (!host.sandboxed) {
		for (auto it = environ; it && *it; ++it) {
			if (std::strncmp(*it, "TMPDIR=", 7) != 0) {
				env.emplace_back(*it);
			}
		}
		env.emplace_back("TMPDIR=" + SharedTempDir());
	} else {
		env.emplace_back("PATH=/usr/bin:/bin");
	}
	for (auto &item : env) {
		envp.push_back(item.data());
	}
	envp.push_back(nullptr);
	auto pid = pid_t();
	const auto code = posix_spawn(&pid, command[0].c_str(), &actions, nullptr, argv.data(), envp.data());
	posix_spawn_file_actions_destroy(&actions);
	::close(child);
	if (code != 0) {
		::close(pair[0]);
		if (log) log(std::string(Mokagram::Lang::Tr("plugins: не удалось запустить хост плагинов: ")) + std::strerror(code));
		return false;
	}
	{
		const auto lock = std::lock_guard(host.mutex);
		host.pid = pid;
	}
	{
		const auto lock = std::lock_guard(host.writeMutex);
		host.sock = pair[0];
	}
	host.alive = true;
	host.spawnedAt = std::chrono::steady_clock::now();
	if (log) log(std::string("plugins: хост плагинов запущен, pid ") + std::to_string(pid)
		+ (host.sandboxed ? " (bwrap)" : " (без песочницы)"));
	return true;
}

// Передаёт хосту скрипты, потом грузит плагины. Зовётся после каждого запуска хоста.
bool BootHost() {
	auto answer = Roundtrip(kFrameBoot, {
		"tl_names", kTlNamesScript,
		"host", kHostScript,
		"fw", kFwScript,
	}, std::chrono::seconds(60));
	if (!answer || answer->empty() || (*answer)[0].empty()) {
		if (_logFn) {
			_logFn(Mokagram::Lang::Tr("python: ошибка загрузки хоста плагинов")
				+ (answer && answer->size() > 1 ? ": " + (*answer)[1] : std::string()));
		}
		return false;
	}
	return true;
}

std::string InitPlugins() {
	return CallStr("_moka_init", {
		PluginsDir(),
		std::string(kSdkScript),
		std::string(kApiScript),
		std::string(kTlScript),
		std::string(kTlSchema),
	});
}

void ClearFilters() {
	const auto lock = std::lock_guard(_filterMutex);
	_filterByPlugin.clear();
	for (auto kind = 0; kind != kHookKinds; ++kind) {
		_filterUnion[kind].clear();
		_filterCount[kind].store(0, std::memory_order_relaxed);
	}
}

void SupervisorLoop() {
	auto &host = H();
	while (true) {
		{
			auto lock = std::unique_lock(host.supMutex);
			host.supCv.wait(lock, [&] { return host.wantSpawn || host.quit; });
			if (host.quit) {
				return;
			}
			host.wantSpawn = false;
		}
		const auto ok = SpawnHost(_logFn);
		const auto restart = !host.firstSpawn;
		{
			const auto lock = std::lock_guard(host.supMutex);
			host.firstSpawn = false;
			host.spawnOk = ok;
			host.spawnDone = true;
		}
		host.supCv.notify_all();
		if (!ok) {
			continue;
		}
		if (restart) {
			// Новый хост нужно загрузить, и это не может делать поток, читающий сокет.
			std::thread([] {
				auto &host = H();
				const auto lock = std::lock_guard(host.initMutex);
				ClearFilters();
				if (!BootHost()) {
					return;
				}
				const auto count = InitPlugins();
				if (_logFn) _logFn("plugins reloaded after host restart: " + count);
				QueueEvent("app_event", R"({"event":"START"})");
			}).detach();
		}
		auto fd = int();
		{
			const auto lock = std::lock_guard(host.writeMutex);
			fd = host.sock;
		}
		ReaderLoop(fd);
		// EOF: хост завершился или убит.
		host.alive = false;
		auto pid = pid_t(-1);
		{
			const auto lock = std::lock_guard(host.mutex);
			pid = host.pid;
			host.pid = -1;
		}
		FailAllWaiters();
		{
			const auto lock = std::lock_guard(host.writeMutex);
			::close(host.sock);
			host.sock = -1;
		}
		if (pid > 0) {
			auto status = 0;
			auto waited = false;
			for (auto i = 0; i != 30 && !waited; ++i) {
				waited = (::waitpid(pid, &status, WNOHANG) != 0);
				if (!waited) {
					if (i == 10) {
						::kill(pid, SIGKILL);
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(100));
				}
			}
		}
		if (_stopping.load()) {
			return;
		}
		auto delay = std::chrono::seconds(0);
		{
			const auto lock = std::lock_guard(host.supMutex);
			if (host.quit) {
				return;
			}
			if (host.manualRestart) {
				host.manualRestart = false;
				host.wantSpawn = true;
				continue;
			}
			if (std::chrono::steady_clock::now() - host.spawnedAt > std::chrono::seconds(60)) {
				host.autoRestarts = 0;
			}
			if (host.autoRestarts >= kAutoRestarts) {
				if (_logFn) _logFn(Mokagram::Lang::Tr("plugins: хост плагинов упал слишком часто, плагины остановлены"));
				continue;
			}
			delay = std::chrono::seconds(1 << host.autoRestarts);
			++host.autoRestarts;
		}
		if (_logFn) _logFn(Mokagram::Lang::Tr("plugins: хост плагинов завершился, перезапуск"));
		std::this_thread::sleep_for(delay);
		const auto lock = std::lock_guard(host.supMutex);
		host.wantSpawn = true;
		host.supCv.notify_all();
	}
}

// Поднимает процесс-хост и грузит в него скрипты. Идемпотентно.
bool EnsureHost(const LogFn &log) {
	auto &host = H();
	if (host.alive.load()) {
		return true;
	}
	if (_logFn == nullptr) {
		_logFn = log;
	}
	{
		auto lock = std::unique_lock(host.supMutex);
		if (host.supStarted) {
			// Дальше хост поднимает только супервизор (авто) или RestartHost.
			return false;
		}
		host.supStarted = true;
		std::thread(SupervisorLoop).detach();
		host.spawnDone = false;
		host.wantSpawn = true;
		host.supCv.notify_all();
		host.supCv.wait_for(lock, std::chrono::seconds(30), [&] { return host.spawnDone; });
		if (!host.spawnOk) {
			return false;
		}
	}
	const auto lock = std::lock_guard(host.initMutex);
	return BootHost();
}

void StopHost() {
	auto &host = H();
	{
		const auto lock = std::lock_guard(host.supMutex);
		host.quit = true;
	}
	host.supCv.notify_all();
	// Закрытый сокет завершает хост; ждать его добровольного выхода не будем дольше секунды.
	{
		const auto lock = std::lock_guard(host.writeMutex);
		if (host.sock >= 0) {
			::shutdown(host.sock, SHUT_RDWR);
		}
	}
	for (auto i = 0; i != 10 && host.alive.load(); ++i) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}
	if (host.alive.load()) {
		KillHost();
	}
}

#endif // MOKA_PLUGINS_PROCESS

#ifndef MOKA_PLUGINS_PROCESS
PyObject *NativeCall(PyObject *, PyObject *args) {
	const char *op = nullptr;
	const char *json = nullptr;
	if (!PyArg_ParseTuple(args, "ss", &op, &json)) {
		return nullptr;
	}
	const auto opName = std::string(op);
	const auto jsonText = std::string(json);
	auto result = std::string();
	Py_BEGIN_ALLOW_THREADS
	result = HandleNative(opName, jsonText);
	Py_END_ALLOW_THREADS
	return PyUnicode_FromStringAndSize(
		result.data(),
		static_cast<Py_ssize_t>(result.size()));
}

PyMethodDef NativeMethod = {
	"_moka_native",
	NativeCall,
	METH_VARARGS,
	Mokagram::Lang::Tr("Вызов клиента из хоста плагинов"),
};

bool LoadHelpers() {
	if (_helpersLoaded) {
		return true;
	}
	const auto module = PyImport_AddModule("__main__");
	const auto globals = PyModule_GetDict(module);
	const auto native = PyCFunction_NewEx(&NativeMethod, nullptr, nullptr);
	if (!native) {
		PyErr_Clear();
		return false;
	}
	PyDict_SetItemString(globals, "_moka_native", native);
	Py_DECREF(native);
	if (PyRun_SimpleString(kTlNamesScript) != 0
		|| PyRun_SimpleString(kHostScript) != 0
		|| PyRun_SimpleString(kFwScript) != 0) {
		return false;
	}
	_helpersLoaded = true;
	return true;
}

// RAII для GIL.
struct Gil {
	Gil() : state(PyGILState_Ensure()) {
	}
	~Gil() {
		PyGILState_Release(state);
	}
	PyGILState_STATE state;
};
#endif // !MOKA_PLUGINS_PROCESS

std::string Hex(const std::string &bytes) {
	static const char digits[] = "0123456789abcdef";
	auto result = std::string();
	result.reserve(bytes.size() * 2);
	for (const auto c : bytes) {
		const auto byte = static_cast<unsigned char>(c);
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 15]);
	}
	return result;
}

// Единственный поток, который выполняет наблюдающие хуки. Главный поток Python не трогает.
void WorkerLoop() {
	while (true) {
		auto task = HookTask();
		{
			auto lock = std::unique_lock(_queueMutex);
			_queueCv.wait(lock, [] {
				return _stopping.load() || !_queue.empty();
			});
			if (_stopping.load()) {
				return;
			}
			task = std::move(_queue.front());
			_queue.pop_front();
		}
		try {
			if (task.kind == kEventKind) {
				const auto split = task.bytes.find('\n');
				const auto gil = Gil();
				CallStr("_moka_event", {
					task.bytes.substr(0, split),
					task.bytes.substr(split + 1),
				});
				Drain(_logFn);
				continue;
			}
			const auto gil = Gil();
			CallStr("_moka_hook_observe", {
				kKindNames[task.kind],
				std::to_string(task.id),
				Hex(task.bytes),
			});
			Drain(_logFn);
		} catch (...) {
		}
	}
}

void StartWorker() {
	if (!_worker.joinable()) {
		_stopping = false;
		_worker = std::thread(WorkerLoop);
	}
}

} // namespace

#ifndef MOKA_PLUGINS_PROCESS
std::string SharedTempDir() {
	return {};
}

void SetSandboxExcluded(std::vector<std::string>) {
}
#endif

std::string PluginsDir() {
	if (const auto env = std::getenv("MOKAGRAM_PLUGINS_DIR")) {
		if (*env) {
			return env;
		}
	}
	const auto home = std::getenv("HOME");
	return std::string(home ? home : ".") + "/.local/share/mokagram/plugins";
}

// Каталог плагинов без единой записи: движок не нужен, Python не поднимаем.
[[nodiscard]] bool HasInstalledPlugins() {
	auto error = std::error_code();
	for (const auto &entry : std::filesystem::directory_iterator(PluginsDir(), error)) {
		if (!entry.path().filename().string().starts_with('.')) {
			return true;
		}
	}
	return false;
}

int StartEngine(LogFn log);

int Start(LogFn log) {
	if (!HasInstalledPlugins()) {
		if (log) log("plugins: каталог пуст, движок поднимется при установке плагина");
		return 0;
	}
	return StartEngine(std::move(log));
}

int StartEngine(LogFn log) {
	if (const auto env = std::getenv("MOKAGRAM_PLUGINS")) {
		if (std::string(env) == "0") {
			if (log) log(Mokagram::Lang::Tr("plugins: отключены переменной MOKAGRAM_PLUGINS=0"));
			return 0;
		}
	}
#ifdef MOKA_PLUGINS_PROCESS
	if (!_started) {
		static auto bridgeInstalled = false;
		_logFn = log;
		// Мост ставим до запуска хоста: он же сообщает песочнице, какие папки клиента закрыть.
		if (!std::exchange(bridgeInstalled, true)) {
			InstallBridge();
		}
	}
	if (!EnsureHost(log)) {
		if (log) log(Mokagram::Lang::Tr("python: ошибка загрузки хоста плагинов"));
		return -1;
	}
	if (_started) {
		return 0;
	}
	_started = true;
#else
	if (!EnsurePython(log)) {
		return -1;
	}
	const auto gil = Gil();
	if (!LoadHelpers()) {
		if (log) log(Mokagram::Lang::Tr("python: ошибка загрузки хоста плагинов"));
		return -1;
	}
	if (_started) {
		return 0;
	}
	_started = true;
	_logFn = log;
	InstallBridge();
#endif
	StartWorker();
#ifdef MOKA_PLUGINS_PROCESS
	const auto count = [&] {
		const auto lock = std::lock_guard(H().initMutex);
		return InitPlugins();
	}();
#else
	const auto count = CallStr("_moka_init", {
		PluginsDir(),
		std::string(kSdkScript),
		std::string(kApiScript),
		std::string(kTlScript),
		std::string(kTlSchema),
	});
#endif
	Drain(log);
	QueueEvent("app_event", R"({"event":"START"})");
	if (log) log("plugins loaded: " + count);
	return count.empty() ? -1 : std::atoi(count.c_str());
}

std::string ParseMetadataJson(const std::string &source) {
#ifdef MOKA_PLUGINS_PROCESS
	if (!EnsureHost(nullptr)) {
		return R"({"meta":{},"detected":[],"error":"Python недоступен"})";
	}
	const auto gil = Gil();
	auto json = CallStr("_moka_parse", { source });
#else
	if (!EnsurePython(nullptr)) {
		return R"({"meta":{},"detected":[],"error":"Python недоступен"})";
	}
	const auto gil = Gil();
	auto json = std::string();
	if (LoadHelpers()) {
		json = CallStr("_moka_parse", { source });
	}
#endif
	if (json.empty()) {
		json = R"({"meta":{},"detected":[],"error":"не удалось разобрать файл"})";
	}
	return json;
}

std::string RequestInfoJson(const std::string &encoded) {
	const auto gil = Gil();
	return CallStr("_moka_request_info", { encoded });
}

std::string LoadFile(const std::string &fileName, LogFn log) {
	if (!_started && StartEngine(log) < 0) {
		return {};
	}
	const auto gil = Gil();
	const auto id = CallStr("_moka_load_file", { fileName });
	Drain(log);
	return id;
}

std::string ListJson() {
	if (!_started) {
		return "[]";
	}
	const auto gil = Gil();
	const auto json = CallStr("_moka_list", {});
#ifdef MOKA_PLUGINS_PROCESS
	// Хост упал или завис: показываем последний список, но с пометкой «ошибка».
	auto &host = H();
	if (!json.empty()) {
		const auto lock = std::lock_guard(host.listMutex);
		host.lastList = json;
		return json;
	}
	auto cached = std::string();
	{
		const auto lock = std::lock_guard(host.listMutex);
		cached = host.lastList;
	}
	auto list = QJsonDocument::fromJson(QByteArray::fromStdString(cached)).array();
	for (auto i = 0; i != list.size(); ++i) {
		auto item = list[i].toObject();
		item.insert("state", "error");
		item.insert("error", QString::fromUtf8(Mokagram::Lang::Tr("хост плагинов не отвечает")));
		list[i] = item;
	}
	return QJsonDocument(list).toJson(QJsonDocument::Compact).toStdString();
#else
	return json.empty() ? "[]" : json;
#endif
}

std::string EffectiveGrantsJson(const std::string &pid) {
	if (!_started) {
		return "[]";
	}
	const auto gil = Gil();
	return CallStr("_moka_effective_grants", { pid });
}

bool EngineEnabled() {
	if (!_started) {
		return true;
	}
	const auto gil = Gil();
	return CallStr("_moka_engine", {}) != "0";
}

bool SetEngineEnabled(bool enabled, LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_set_engine", { enabled ? "1" : "0" });
	Drain(log);
	return result == "true";
}

bool SetPinned(const std::string &id, bool pinned) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	return CallStr("_moka_set_pinned", { id, pinned ? "1" : "0" }) == "true";
}

bool SetEnabled(const std::string &id, bool enabled, LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_set_enabled", { id, enabled ? "1" : "0" });
	Drain(log);
	return result == "true";
}

bool Remove(const std::string &id, LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_remove", { id });
	Drain(log);
	return result == "true";
}

std::string CallHook(
		const std::string &name,
		const std::string &paramsJson,
		LogFn log) {
	if (!_started) {
		return {};
	}
	const auto gil = Gil();
	auto result = CallStr("_moka_hook", { name, paramsJson });
	Drain(log);
	return result;
}

bool CallMessageFilter(const std::string &messageJson, LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_filter", { messageJson });
	Drain(log);
	return result == "1";
}

std::string CallTextHook(const std::string &messageJson, LogFn log) {
	if (!_started) {
		return {};
	}
	const auto gil = Gil();
	auto result = CallStr("_moka_text_hook", { messageJson });
	Drain(log);
	return result;
}

std::string CallBadgeHook(const std::string &messageJson, LogFn log) {
	if (!_started) {
		return {};
	}
	const auto gil = Gil();
	auto result = CallStr("_moka_badge_hook", { messageJson });
	Drain(log);
	return result;
}

void OpenPluginBuilder(LogFn log) {
	if (!_started) {
		return;
	}
	const auto gil = Gil();
	CallStr("_moka_builder", {});
	Drain(log);
}

void RestoreBackupFile(const std::string &path, LogFn log) {
	if (!_started) {
		return;
	}
	const auto gil = Gil();
	CallStr("_moka_restore", { path });
	Drain(log);
}

bool Invoke(
		const std::string &pid,
		const std::string &token,
		const std::string &payloadJson,
		LogFn log) {
	if (!_started || token.empty()) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_invoke", { pid, token, payloadJson });
	Drain(log);
	return result == "true";
}

std::string MenuItemsJson(
		const std::string &location,
		const std::string &ctxJson) {
	if (!_started) {
		return "[]";
	}
	const auto gil = Gil();
	const auto json = CallStr("_moka_menu_items", { location, ctxJson });
	return json.empty() ? "[]" : json;
}

bool MenuItemClick(
		const std::string &pid,
		int index,
		const std::string &ctxJson,
		LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr(
		"_moka_menu_click",
		{ pid, std::to_string(index), ctxJson });
	Drain(log);
	return result == "true";
}

void Shutdown(LogFn log) {
	if (!_started) {
		return;
	}
	// Рабочий поток нужно остановить до того, как мы возьмём GIL: ему он тоже нужен.
	{
		const auto lock = std::lock_guard(_queueMutex);
		_stopping = true;
		_queue.clear();
	}
	_queueCv.notify_all();
	if (_worker.joinable()) {
		_worker.join();
	}
	const auto gil = Gil();
	CallStr("_moka_shutdown", {});
	Drain(log);
#ifdef MOKA_PLUGINS_PROCESS
	StopHost();
#endif
}

void SetBridge(BridgeFn bridge) {
	const auto lock = std::lock_guard(_bridgeMutex);
	_bridge = std::move(bridge);
}

bool HostAlive() {
#ifdef MOKA_PLUGINS_PROCESS
	return H().alive.load();
#else
	return true;
#endif
}

bool RestartHost(LogFn log) {
#ifdef MOKA_PLUGINS_PROCESS
	auto &host = H();
	if (!_started || _stopping.load()) {
		return false;
	}
	if (log) {
		_logFn = log;
	}
	const auto alive = host.alive.load();
	{
		const auto lock = std::lock_guard(host.supMutex);
		if (!host.supStarted) {
			return false;
		}
		host.autoRestarts = 0;
		if (alive) {
			host.manualRestart = true;
		} else {
			host.wantSpawn = true;
		}
	}
	if (alive) {
		KillHost();
	} else {
		host.supCv.notify_all();
	}
	return true;
#else
	return false;
#endif
}

bool Stopping() {
	return _stopping.load();
}

bool HookActive(HookKind kind) {
	const auto index = int(kind);
	return index >= 0
		&& index < kHookKinds
		&& _filterCount[index].load(std::memory_order_relaxed) != 0;
}

bool HookWanted(HookKind kind, uint32_t constructorId) {
	const auto index = int(kind);
	if (index < 0
		|| index >= kHookKinds
		|| _filterCount[index].load(std::memory_order_relaxed) == 0) {
		return false;
	}
	const auto lock = std::lock_guard(_filterMutex);
	return _filterUnion[index].contains(constructorId);
}

void QueueHook(
		HookKind kind,
		uint32_t constructorId,
		const void *data,
		size_t sizeBytes) {
	if (!_started
		|| _stopping.load()
		|| !data
		|| sizeBytes > kMaxHookBytes
		|| ((kind == HookKind::PreRequest || kind == HookKind::PostRequest)
			&& _fromPlugin.load() > 0)) {
		return;
	}
	auto task = HookTask{
		.kind = int(kind),
		.id = constructorId,
		.bytes = std::string(static_cast<const char*>(data), sizeBytes),
	};
	auto dropped = size_t();
	{
		const auto lock = std::lock_guard(_queueMutex);
		if (_queue.size() >= kQueueLimit) {
			_queue.pop_front();
			dropped = ++_dropped;
		}
		_queue.push_back(std::move(task));
	}
	_queueCv.notify_one();
	if (dropped && (dropped % 100) == 1 && _logFn) {
		_logFn(Mokagram::Lang::Tr("plugin: очередь хуков переполнена, отброшено событий: ")
			+ std::to_string(dropped));
	}
}

void QueueEvent(const std::string &name, const std::string &payloadJson) {
	if (!_started || _stopping.load() || payloadJson.size() > kMaxHookBytes) {
		return;
	}
	auto task = HookTask{ .kind = kEventKind, .bytes = name + "\n" + payloadJson };
	{
		const auto lock = std::lock_guard(_queueMutex);
		if (_queue.size() >= kQueueLimit) {
			_queue.pop_front();
		}
		_queue.push_back(std::move(task));
	}
	_queueCv.notify_one();
}

void MarkFromPlugin(bool on) {
	if (on) {
		++_fromPlugin;
	} else if (_fromPlugin.load() > 0) {
		--_fromPlugin;
	}
}

bool IsFromPlugin() {
	return _fromPlugin.load() > 0;
}

std::string RecordInstall(const std::string &fileName, LogFn log) {
	if (!_started && StartEngine(log) < 0) {
		return {};
	}
	const auto gil = Gil();
	const auto id = CallStr("_moka_record_install", { fileName });
	Drain(log);
	return id;
}

bool Approve(const std::string &id, LogFn log) {
	if (!_started) {
		return false;
	}
	const auto gil = Gil();
	const auto result = CallStr("_moka_approve", { id });
	Drain(log);
	return result == "true";
}

void NotifySettingChanged(
		const std::string &key,
		const std::string &valueJson,
		LogFn log) {
	if (!_started) {
		return;
	}
	const auto gil = Gil();
	CallStr("_moka_settings_changed", { key, valueJson });
	Drain(log);
}

} // namespace Mokagram::Plugins
