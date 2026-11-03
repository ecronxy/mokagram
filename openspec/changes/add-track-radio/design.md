# Design

## Context

MusicRow::showMenu serves rows and ShowCurrentTrackMenu serves playback. Related fetches SoundCloud candidates; BuildStream considers history and deduplication. Telegram ids are not SoundCloud ids.

## Technical Approach

One radio launch accepts a copied Track, not a mutable row index. Preserve the chosen seed for the session and load asynchronously. Preserve existing queue/playback until suitable candidates arrive. Then create a queue beginning with the seed and related tracks; preserve position if that seed already plays. Replenish from the original seed, excluding played/queued duplicates and blocked artists. Session identity rejects stale and other-account responses. Unsupported Telegram tracks have an unavailable action with an explanation; never send their ids to SoundCloud or guess a provider match. Reuse native menus and keyboard invocation, without player redesign.

## Risks / Trade-offs

Check Related error versus empty-result contract. Repeated launches invalidate earlier responses. Verify interaction with stream, shuffle and repeat.

## Delivery

See agent-task.md. This is a local OpenSpec assignment, not an ai-tdesktop queue record: that sibling repository is absent. No radio agent has been launched.
