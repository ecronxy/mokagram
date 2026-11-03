# Design

## Context

ChannelsController enumerates broadcasts and FeedSection::refresh repeats that restriction. Categories store complete PeerId strings in the legacy JSON channels field under feed.<session>.categories.

## Technical Approach

Use the native chats-list controller for existing dialogs, loaded archive and Saved Messages, updating as dialogs load. Preserve selected accessible peers before their dialog page appears. Restrict selection to existing accessible conversations, without public discovery. Keep the channels JSON key for compatibility, but call them sources in code and copy. Preserve PeerId precision with strings. Use existing messages.getHistory processing for channel/chat/user peers; identify source and non-broadcast sender. Sort descending by timestamp, peer and message id; deduplicate by peer/message id. Service messages advance pagination without becoming feed cards. Partial errors retain successful results; FLOOD_WAIT stops the pass. Loading leaves read state unchanged.

## Risks / Trade-offs

Private chats are opt-in. Saved Messages uses the self peer. Guard channel-specific operations. Lost access skips a source without deleting the rest and contributes to the visible failure count. Missing saved peers trigger dialog loading and refresh as they become available; section snapshots retain pending peers and unavailable-source counts. Preserve unrelated working-tree changes.

## Verification

Mixed category with a channel, basic group, supergroup, private chat and Saved Messages; archive; partial dialog loading; restart; legacy JSON; account switch; equal message ids across peers; inaccessible source; partial errors; unchanged read state.
