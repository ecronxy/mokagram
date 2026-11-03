# Verification

Implemented locally on 2026-11-03 in music modules and the required Russian/English localization. This remains an ordinary local assignment, not an ai-tdesktop queue record. Existing unrelated changes were preserved.

The existing Related API collapses network errors into empty collections. Radio uses LoadRelatedPage instead, retaining explicit errors and next_href pagination. BuildStream mixes taste/history seeds, so radio does not use it: each session retains the selected track and its identity exclusion set. Playback and the prior queue remain intact until related candidates and, when necessary, the seed audio file are ready.

## Completed checks

- Clang syntax checks passed for mokagram_music_ui.cpp, mokagram_music_library.cpp and mokagram_soundcloud.cpp using the configured Release include paths and precompiled-header source, without generating app objects or linking an application.
- A standalone C++/Qt harness extracted the actual radio lifecycle functions and identity-normalization functions from the working sources. With controlled provider, download and player substitutes, it passed error/empty responses, unsupported and blocked seeds, resolve/download failures, successful queue replacement, position preservation for the playing seed, fixed-seed pagination, ID/metadata duplicates, blocked artists, manual queue insertion during a request, removed-track exclusion, repeated launches, playback/account changes, bounded empty-page scans, repeated cursors, exhaustion, and radio navigation with shuffle/repeat-all enabled.
- A second standalone C++/Qt harness exercised the actual LoadRelatedPage function with a controlled ApiGet and track parser: invalid provider IDs, foreign or mismatched cursor endpoints, network failure, malformed JSON, missing collections, empty success, unavailable tracks, pagination and stale client_id removal all passed.
- An offscreen Qt Widgets harness exercised the actual Row::keyPressEvent method: Menu and Shift+F10 dispatch keyboard context-menu events at the row centre, and plain F10 does not open the menu. Both native menu builders call the shared radio action; the player dialog also handles keyboard invocation.
- git diff --check passed. Added user-visible strings have English translations. No feed source was edited by this assignment.

## Remaining manual validation

The user subsequently requested a build. The local Release build completed with BUILD SUCCEEDED; the freshly linked mokaGram.app was installed into /Applications, ad-hoc signed, verified and relaunched successfully. No live SoundCloud/UI playback test was run. Check both menus in the installed app, real provider pagination and playback through the queue boundary, repeat-one, language switching, and account switching while requests are in flight. The controlled harnesses verify state transitions and error contracts; they do not validate provider availability or the complete player integration.
