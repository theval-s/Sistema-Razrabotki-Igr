# Asset event correlation contract

`AssetLoadedEvent` and `AssetFailedEvent` include defaulted `source`, `slot` and `generation` fields. Existing aggregate initializers remain source-compatible, but consumers must rebuild for the changed event layout.

Match **source + kind + slot + generation** against the current typed handle and its owning `AssetSystem`. `id` is a name hash, not a catalog UUID or request ID; `name` is the owned normalized request name, including shader variant identity. The tuple identifies the load attempt even after failure retires its handle. Null source or an invalid slot denotes an uncorrelated legacy/synthetic event; exact-match consumers ignore it.

The source pointer is non-owning, equality-only and engine-thread-only. It identifies the `AssetSystem` only during that object's lifetime: disconnect and discard caches before destruction, never dereference it from an event or serialize it, and do not assume an address identifies a later engine. Global and scene systems can otherwise have identical handles. Reinitializing the same system preserves its source identity; generation retirement distinguishes old records. Clear scene presentation state on scene replacement.

Completion events carry the original handle's slot and generation. Failure is published after the record is failed/retired; success is published after the value becomes Ready. Stale jobs are suppressed, and event fields are captured before dispatch so reentrant listeners can retry safely. Blocking loads follow the same correlation, including the missing-job failure path. Rejection before a handle exists and cancellation on shutdown/unload do not create a correlated completion event.

Successful live material reload publishes a loaded event for the existing Ready handle. Failed live reload retains the old Ready value and reports failure through its return/error/log path rather than publishing a misleading failed-load event. This tuple does not sequence deferred replay of multiple reloads for the same handle.

The editor subscribes before scene requests and caches owned failure details for scene renderer references, matching the full attempt identity and pruning on removal, replacement or repair. A blocking event can arrive before a component is installed. Late subscribers cannot recover missed details from a retired record; consult the existing log. This cache is presentation state, not another asset registry.
