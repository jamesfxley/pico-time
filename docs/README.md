# pico-time documentation

| Document | What it is | Read it when |
|---|---|---|
| [traceability-exceptions.txt](traceability-exceptions.txt) | Requirements no source file can cite (EN-02.7). | Reviewing uncitable requirements. |
| [orientation.md](orientation.md) | How this repository is built and gated, how it is laid out, and the repository's invariants and traps | Working in it for the first time, or before an edit |
| [technical-specification.md](technical-specification.md) | **Normative.** Requirements (`FR`/`NR`/`VR`/`AR`), gates, traceability matrix | Changing behaviour, or asking whether something is guaranteed or incidental |
| [architecture.md](architecture.md) | Descriptive. How the implementation is put together — the host/firmware split, the wall clock's publication protocol, the attempt slot pool, lwIP configuration | Working out where a change belongs |
| [usage-guide.md](usage-guide.md) | Practical use beyond the README: linking it, synchronizing and re-synchronizing, the plausibility window, reading the clock from an ISR | Building an application on the library |
| [deviations.md](deviations.md) | Standing (scope-wide) deviations from the coding standard, each with a rationale, a risk assessment and a review date | Adding or reviewing a suppression |
| [deviation-budget.txt](deviation-budget.txt) | One suppression ceiling per language, ratcheting down. These only ever ratchet down | Same |
| [requirement-prefixes.txt](requirement-prefixes.txt) | The pinned prefix set, checked by review (EN-02.4) | Adding a requirement under a new prefix |
| [GitHub Releases](https://github.com/jamesfxley/pico-time/releases) | What changed in each release, and what to change in your code when an API broke | Upgrading across a release |

The specification is the only normative document here. `architecture.md`
describes what is, and work deliberately not done is an issue in the tracker
rather than a document in the tree (`EN-03.7`).

A behaviour claimed in `README.md` or `docs/orientation.md` but absent from the
specification is not a commitment, and a requirement is not satisfied until the
matrix names what establishes it.

The repository `README.md` is the surface — the API and what a server has to
prove. `usage-guide.md` is the other half: wiring it into an image.

The API reference is generated from the headers rather than written by hand, so
it cannot drift from the declarations it documents:

```sh
doxygen docs/Doxyfile      # -> docs/api/html/index.html (gitignored)
```

CI builds it on every push to `main` and every pull request, fails on an
undocumented public entity.

## Conventions

- Requirement IDs are stable and never renumbered. Code and tests cite the ID,
  never a section number: IDs survive a reorganisation of this document and
  section numbers do not. Review checks requirement citations (EN-02.1);
  `requirement-prefixes.txt` pins their prefix set (EN-02.4).
- The specification states what is true now — no dates, no version history, no
  measured values. It may state a normative gate ("within a CI-enforced
  budget") but not a measurement; a measured figure is a run record and lives
  beside the rig that produced it (`EN-03.2`).
- A behaviour change lands as one change: the specification row, a host test,
  and the affected documents together.
