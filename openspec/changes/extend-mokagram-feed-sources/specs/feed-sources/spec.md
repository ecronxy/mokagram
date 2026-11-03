# Spec Delta

## Purpose

The user wants channels, basic groups, supergroups, private chats and Saved Messages in a manually selected feed.

## ADDED Requirements

### Requirement: Mixed selection
The system SHALL allow manual selection of up to 32 unique sources including channels, groups, private chats and Saved Messages.

#### Scenario: Mixed category
- **WHEN** the user saves different conversation types
- **THEN** all selected accessible sources contribute messages

#### Scenario: Partial dialogs
- **WHEN** dialogs are still loading
- **THEN** the list updates, offers Saved Messages and retains previous selection

### Requirement: Compatibility
The system SHALL preserve legacy categories and isolate settings and content by account.

#### Scenario: Legacy category
- **WHEN** an old category opens
- **THEN** its id, name and selected channels remain intact

#### Scenario: Account switch
- **WHEN** the account changes
- **THEN** only that account sources and categories appear

### Requirement: Attribution and identity
The system SHALL distinguish peer/message-id pairs, show source and available sender, open the original message and leave source read state unchanged.

#### Scenario: Equal ids
- **WHEN** two peers have an equal message id
- **THEN** both messages remain present and navigate correctly

#### Scenario: Group sender
- **WHEN** a participant message is shown
- **THEN** the source group and available sender name are identifiable

#### Scenario: Failed source
- **WHEN** one source fails
- **THEN** successful results remain and a clear error is shown
