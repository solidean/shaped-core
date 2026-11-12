---
created: 2026-11-06
priority: normal
tags: [xr, ar]
parent: epic-xr
blocked-by: [feat-xr-capabilities]
---

# Anchors: created, located, persisted and restored on one headset

Done when `xr-cube` is still on the table in the next session.
Design: [AR capabilities](../../libs/graphics/shaped-xr/docs/design.md#ar-capabilities).

## Wanted

- `session::create_anchor`, returning `cc::shared_async<sxr::anchor>`.
- `frame.pose_of(anchor)`, at display time, none while lost.
- `session::persist`, giving an `sxr::anchor_id` the app stores, and `session::restore` from one.
- Natively over `XR_FB_spatial_entity` and `XR_META_spatial_entity_persistence`, whose FB storage predecessor is obsolete.

## Decided

Sharing between headsets is out: it is a different feature, with its own account and network story.

## Unknown

- Whether an anchor survives the session ending, which a headset coming off causes; the proposal assumes an app restores on resume.
- Whether `USE_ANCHOR_API` needs a runtime grant beyond the manifest.
