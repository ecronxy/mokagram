// Mokagram: хранилище истории (удалённые сообщения и старые версии правок).
// Одна SQLite-база на аккаунт, запись и чтение только в фоновых потоках.
#pragma once

#include "base/basic_types.h"
#include "base/weak_ptr.h"
#include <rpl/event_stream.h>
#include "base/flat_map.h"
#include "base/flat_set.h"
#include "data/data_msg_id.h"

#include <QtCore/QString>
#include <QtCore/QByteArray>

#include <memory>
#include <vector>

namespace Mokagram::History {

enum class Kind : int {
	Deleted = 1,
	Edited = 2,
};

// Значение без указателей на HistoryItem: делается в потоке интерфейса.
struct Snapshot {
	uint64 peer = 0;
	int64 topic = 0;
	int64 id = 0;
	Kind kind = Kind::Deleted;
	int32 date = 0;
	int32 event = 0; // Когда удалили или изменили.
	uint64 from = 0;
	QString text;
	QByteArray entities;
	QString media; // Короткая подпись медиа: «Фото», «Стикер» и так далее.
	QByteArray tl; // MTPMessageMedia целиком (фото, голосовые и т.д.).
};

struct Counts {
	int deleted = 0;
	int edited = 0;
};

struct Limits {
	int days = 30;       // 0 = бессрочно.
	int megabytes = 256; // 0 = без ограничения.
	int perChat = 5000;  // 0 = без ограничения.
};

struct PageQuery {
	uint64 peer = 0;
	Kind kind = Kind::Deleted;
	int64 id = 0;          // Для правок одного сообщения, иначе 0.
	int32 beforeEvent = 0; // Курсор (keyset), 0 = с самого свежего.
	int64 beforeId = 0;
	int64 minId = 0;       // Только id >= minId (для призраков), 0 = без границы.
	int32 since = 0;       // Только события не раньше since, 0 = за всё время.
	QString mediaLike;     // Маска подписи медиа для LIKE, пусто = любые.
	int limit = 50;
};

class Store final : public base::has_weak_ptr {
public:
	explicit Store(const QString &path);
	~Store();

	// Не блокирует: кладёт снимок в очередь фонового писателя.
	void push(Snapshot snapshot);
	void pruneNow(Limits limits);
	// Лимиты читаются в потоке интерфейса при каждой N-й записи.
	void setLimitsProvider(Fn<Limits()> provider);

	void removeOne(uint64 peer, int64 id);
	void clearPeer(uint64 peer);
	void clearAll();

	// Счётчики и подсказки живут в памяти, к диску интерфейс не ходит.
	[[nodiscard]] Counts counts(uint64 peer) const;
	[[nodiscard]] const base::flat_map<uint64, Counts> &allCounts() const;
	[[nodiscard]] bool hasRevisions(FullMsgId id) const;
	// Лениво подгружает id сообщений чата, у которых есть версии.
	void ensureRevisionsLoaded(uint64 peer);

	// Результат приходит в поток интерфейса, если Store ещё жив.
	void loadPage(
		PageQuery query,
		Fn<void(std::vector<Snapshot>)> done);

	// Срабатывает после записи пачки и после очистки.
	[[nodiscard]] rpl::producer<> changes() const;

private:
	struct Impl;

	void reloadCounts();

	const std::unique_ptr<Impl> _impl;
	base::flat_map<uint64, Counts> _counts;
	base::flat_set<uint64> _revisionsLoaded;
	base::flat_set<FullMsgId> _revisions;
	rpl::event_stream<> _changes;
	int _sincePrune = 0;
	Fn<Limits()> _limits;

};

} // namespace Mokagram::History
