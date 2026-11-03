# Verification

- OpenSpec strict validation: passed for this change.
- git diff --check: passed.
- code-health check against HEAD: passed with warnings from existing changes outside this extension: growth of `mokagram_music_ui.cpp` and an unused `feedCategoryMinWidth` style value. Those unrelated edits were left intact.
- Source review: legacy category ids, names and the JSON `channels` key remain unchanged; peer ids remain decimal strings; settings use the session-specific key. Selection is limited to 32 unique sources and uses native dialog updates, archive loading and Saved Messages.
- History review: requests use the selected peer input; existing-message processing accepts a nullable channel, processes returned peers and does not mark history read. Cards and navigation use peer/message-id pairs; sorting uses descending date, peer and message id. Service messages advance the history offset.
- Standalone C++ regression: passed using the actual `refresh()`, `resolvePendingSources()` and `loadMore()` method bodies with fake session, API and UI dependencies. Covered all five source types, unavailable groups/channels, unknown peers, partial failures across pagination, main/archive dialog requests, delayed source restoration, scroll restoration, category changes and empty selection. This does not compile the complete feed translation unit or validate real RPC/UI behavior.
- Missing saved peers are retried as dialogs arrive. Pending peers and unavailable-source counts survive section snapshots; missing or inaccessible sources contribute to visible errors. Refresh remains available when every source is unavailable.
- Full application compilation and installation: not run; repository instructions require a user build request.
- Live checks remain pending: mixed categories and legacy JSON after restart, switching accounts, equal message ids across peers, real RPC errors, source read state, keyboard navigation and sender layout.
- Existing unrelated feed presentation, music, plugin and submodule edits were preserved. No commit or publication performed.
