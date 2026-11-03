# Spec Delta

## Purpose

The feed must stay readable in narrow panes and wide windows while preserving mokaGram identity.

## ADDED Requirements

### Requirement: Pane adaptation
The feed SHALL keep cards, filters and actions inside the pane at the supported client minimum.

#### Scenario: Narrow pane
- **WHEN** the pane narrows
- **THEN** padding becomes compact, actions wrap and media retain aspect ratio

#### Scenario: Wide pane
- **WHEN** the pane widens
- **THEN** a single centered reading column retains bounded width

### Requirement: Readable content
The feed SHALL preserve native typography, expandable long messages and access to full truncated source names.

#### Scenario: Long source name
- **WHEN** a source name does not fit
- **THEN** the full name is available through a tooltip

### Requirement: Native identity
The feed SHALL use mokaGram theme tokens, fonts and icons and visible keyboard focus.

#### Scenario: Theme and scale
- **WHEN** theme or UI scale changes
- **THEN** controls remain reachable and content hierarchy is retained
