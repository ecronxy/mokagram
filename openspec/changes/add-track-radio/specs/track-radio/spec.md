# Spec Delta

## Purpose

The user wants radio from a selected track through its right-click menu as a separate agent assignment.

## ADDED Requirements

### Requirement: Context menu
The system SHALL offer a localized radio action through mouse and keyboard context menus for supported track rows and the current track.

#### Scenario: Selected row
- **WHEN** radio launches from another row
- **THEN** that row remains the seed even if results change

#### Scenario: Unsupported track
- **WHEN** a Telegram track has no verified provider identity
- **THEN** the action is unavailable with an explanation and no invalid provider request occurs

### Requirement: Safe transition
The system SHALL preserve playback and queue until suitable candidates load and retain them on failure or empty results.

#### Scenario: Success
- **WHEN** suitable candidates arrive
- **THEN** the queue starts with the seed, preserving its position if already playing

#### Scenario: Failure
- **WHEN** loading fails or returns no usable tracks
- **THEN** the existing queue remains and a clear result is shown

### Requirement: Continuation
The system SHALL replenish from the original seed, exclude duplicates and blocked artists and ignore stale responses.

#### Scenario: Repeated launch
- **WHEN** another radio starts before the first request finishes
- **THEN** only the latest session changes the queue

#### Scenario: Replenishment
- **WHEN** radio nears queue end
- **THEN** new suitable tracks are added without session repeats
