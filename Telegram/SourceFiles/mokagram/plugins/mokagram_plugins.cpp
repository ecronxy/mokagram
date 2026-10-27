// This file is part of mokaGram, https://github.com/ecronxy/mokagram
// (c) 2026 @ecronx (https://t.me/ecronx). GPLv3 with the attribution term
// of MOKAGRAM_NOTICE.md (section 7(b)): keep this notice when copying.
// mokagram-id: c69c989a
// Mokagram: хост плагинов (встроенный Python).
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include "mokagram/lang/mokagram_lang.h"

#include "mokagram/plugins/mokagram_plugins.h"

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
#include "mokagram/plugins/py/tl_schema.inc"

constexpr auto kQueueLimit = size_t(256);
constexpr auto kMaxHookBytes = size_t(1024 * 1024);
constexpr auto kAuditLimit = std::uintmax_t(5 * 1024 * 1024);
constexpr auto kHookKinds = 4;
constexpr const char *kKindNames[kHookKinds] = {
	"0", "1", "2", "3"
};

bool _started = false;
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

bool EnsurePython(const LogFn &log) {
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

std::string PluginsDir() {
	if (const auto env = std::getenv("MOKAGRAM_PLUGINS_DIR")) {
		if (*env) {
			return env;
		}
	}
	const auto home = std::getenv("HOME");
	return std::string(home ? home : ".") + "/.local/share/mokagram/plugins";
}

int Start(LogFn log) {
	if (const auto env = std::getenv("MOKAGRAM_PLUGINS")) {
		if (std::string(env) == "0") {
			if (log) log(Mokagram::Lang::Tr("plugins: отключены переменной MOKAGRAM_PLUGINS=0"));
			return 0;
		}
	}
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
	StartWorker();
	const auto count = CallStr("_moka_init", {
		PluginsDir(),
		std::string(kSdkScript),
		std::string(kApiScript),
		std::string(kTlScript),
		std::string(kTlSchema),
	});
	Drain(log);
	QueueEvent("app_event", R"({"event":"START"})");
	if (log) log("plugins loaded: " + count);
	return count.empty() ? -1 : std::atoi(count.c_str());
}

std::string ParseMetadataJson(const std::string &source) {
	if (!EnsurePython(nullptr)) {
		return R"({"meta":{},"detected":[],"error":"Python недоступен"})";
	}
	const auto gil = Gil();
	auto json = std::string();
	if (LoadHelpers()) {
		json = CallStr("_moka_parse", { source });
	}
	if (json.empty()) {
		json = R"({"meta":{},"detected":[],"error":"не удалось разобрать файл"})";
	}
	return json;
}

std::string LoadFile(const std::string &fileName, LogFn log) {
	if (!_started && Start(log) < 0) {
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
	return json.empty() ? "[]" : json;
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
}

void SetBridge(BridgeFn bridge) {
	const auto lock = std::lock_guard(_bridgeMutex);
	_bridge = std::move(bridge);
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
	if (!_started && Start(log) < 0) {
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
