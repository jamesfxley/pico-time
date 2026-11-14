# pico-time documentation

| Document | Purpose | Read it when |
|---|---|---|
| [traceability-exceptions.txt](traceability-exceptions.txt) | Requirements with no source citation | Reviewing uncitable requirements |
| [orientation.md](orientation.md) | Local builds, layout and constraints | Working in this repository |
| [technical-specification.md](technical-specification.md) | Normative requirements, gates and traceability | Checking guarantees or changing behaviour |
| [architecture.md](architecture.md) | Implementation and design reasons | Deciding where a change belongs |
| [usage-guide.md](usage-guide.md) | Firmware configuration and clock use | Building an application |
| [future-work.md](future-work.md) | Deferred work and evidence gaps | Checking planned work or proposing a feature |
| [deviations.md](deviations.md) | Approved scope-wide rule deviations | Reviewing suppressions |
| [deviation-budget.txt](deviation-budget.txt) | Suppression ceilings | Checking deviation budgets |
| [requirement-prefixes.txt](requirement-prefixes.txt) | Requirement prefixes | Adding a prefix |
| [GitHub Releases](https://github.com/jamesfxley/pico-time/releases) | Changes and migration steps | Upgrading |

The specification owns guarantees. The [root README](../README.md) introduces
the API, and the usage guide covers firmware setup. Deferred work belongs in
`future-work.md`; the issue tracker holds defects.

The API reference is generated from public declarations:

```sh
doxygen docs/Doxyfile      # -> docs/api/html/index.html (gitignored)
```

The `api-docs` gate fails on undocumented public entities. Documentation and
citation policy is in the [pinned standard](../infra/house/standard/engineering/03-documentation.md).

## Conventions

Requirement citations follow [the traceability rules](../infra/house/standard/engineering/02-spec-and-traceability.md).
The specification may state a normative gate but carries no measured values;
measurements belong in run records under
[the documentation rules](../infra/house/standard/engineering/03-documentation.md).
