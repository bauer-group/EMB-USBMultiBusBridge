# Architecture Decision Records

ADRs document *why* a non-obvious decision was made. They are intended
to be read in a few minutes, link to a specific code change or state,
and are appended-to rather than rewritten.

Format follows [Michael Nygard's ADR template](https://github.com/joelparkerhenderson/architecture-decision-record/blob/main/locales/en/templates/decision-record-template-by-michael-nygard/index.md)
with minor BAUER GROUP additions (one-line status tag, links to the
design drafts).

| № | Title                                            | Status     |
|---|--------------------------------------------------|------------|
| 0001 | [USB protocol framing choice](0001-protocol-choice.md)          | Accepted |
| 0002 | [Linux driver bus pattern — `auxiliary_bus`](0002-driver-bus-choice.md) | Accepted |
| 0003 | [Language choice — Rust in kernel and userspace](0003-language-choice.md) | Accepted |
| 0004 | [Driver-priority mode via dual USB configuration](0004-driver-priority-mode.md) | Superseded by 0005 |
| 0005 | [Driver-optional coexistence](0005-driver-optional-coexistence.md) | Accepted |
| 0006 | [UART via CDC-ACM only, CAN via gs_usb only](0006-uart-cdc-only-can-gs-usb-only.md) | Accepted |

Adding a new ADR? Copy the template from `0001-…` and bump the number.
Every ADR changes the repository — reference it from the code it
affects and from the design drafts under `docs/drafts/` when it
supersedes one of their paragraphs.
