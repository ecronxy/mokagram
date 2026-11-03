# Design

## Context

Source inspection found fixed padding, an 800-unit reading column and no full-name tooltip. Existing uncommitted media/reaction work is the baseline and must survive. Live visual review is outstanding; no decorative gradients or glows were found in feed styling. Message and reaction emoji are content and remain.

## Direction

- Product: a shared reading feed of selected conversations inside mokaGram.
- Character: calm, composed, slightly warm.
- World: the familiar messenger window and a personal reading collection.
- Moments: real media and avatars on quiet surfaces from the current theme.
- Signature: a consistent source line with avatar, name and time.

## Technical Approach

Apply better-layout, better-ui, better-typography and unslop through native `.style` tokens. Follow spacing, color, icons, copy, type, layout, details, media and motion passes. Compact padding when content stops fitting, wrap actions by measured width and preserve a bounded single reading column. Keep native fonts, themes, icons and interaction density. Full source names use tooltips; long text remains expandable. No new decoration or routine entrance motion.

## Risks / Trade-offs

Source expansion shares files: apply patches sequentially, keep acceptance independent. Translation and UI scale change wrapping thresholds.

## Verification

Check pane widths 320/480/800/1200 logical units and supported client minimum; light/dark themes, Russian/English, long names, RTL, UI scale 100/150/200%, focus, loading, empty and error states. Record unperformed checks. Build only Release when requested and install through mokagram-build.

Unslop principles adapted from Damir Sokolovsky ([@heydamir](https://x.com/heydamir)), CC BY 4.0.
