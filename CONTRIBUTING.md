# Contributing

## Commit messages are the release notes

There is no `CHANGELOG.md`. The release workflow builds the GitHub release body
from the commits between the previous tag and the new one, so **a commit subject
is user-facing text**. A subject nobody can read becomes a release note nobody
can read.

Format is [Conventional Commits](https://www.conventionalcommits.org/):

```
<type>(<scope>)!: <subject>

<body>
```

| Part | Rule |
|---|---|
| `type` | one of the table below — it decides the release-note section |
| `scope` | optional; use the target it touches: `cssharp`, `swiftly`, `metamod`, `db`, `ci`, `docs` |
| `!` | append to the type when the change breaks compatibility — it gets its own section at the top |
| subject | imperative, lowercase, no trailing dot, **English** |

| Type | Release section | Use for |
|---|---|---|
| `feat` | Added | new behaviour a server owner would notice |
| `fix` | Fixed | a defect that was reachable in a shipped build |
| `perf` | Performance | same behaviour, less cost |
| `refactor` | Changed | internal restructuring, no behaviour change |
| `docs` | Documentation | README, `docs/`, comments |
| `build` | Build | build scripts, dependencies, packaging |
| `ci` | CI | workflows |
| `test` | Tests | tests only |
| `chore` | Chores | anything else with no user-visible effect |

A commit that does not match the format is **not dropped** — it lands under
"Other". Silently losing a change from the notes is worse than showing it
without a category, but "Other" is a sign the subject needs fixing.

The body is where the reasoning goes: *why* this change, what the alternative
was, what breaks if someone undoes it. The subject is for the reader of the
release, the body is for the person who runs `git blame` in two years.

## Language

English everywhere: commits, documentation, code comments in the C# targets.

Two deliberate exceptions:

* `README.ru.md` — the Russian translation of the user-facing README.
* The `metamod/` target's C++ comments and its `README.md` are in Russian, as is
  `Settings.json`'s inline commentary, matching the audience that runs it.

## Before you push

```bash
export PATH="$HOME/.dotnet:$PATH"

dotnet test cssharp/ConnectHistory.sln
dotnet test swiftly/ConnectHistory.sln

cd metamod && make -f Makefile.tests -j8 && ./build-tests/ch_tests
make -f Makefile.tests noexcept-check
```

MySQL integration tests are skipped unless `CH_TEST_MYSQL` is set — without it
they prove nothing:

```bash
docker run --rm -d -p 3399:3306 -e MYSQL_ROOT_PASSWORD=test -e MYSQL_DATABASE=ch mysql:8
export CH_TEST_MYSQL="server=127.0.0.1;port=3399;user=root;password=test;database=ch"
```

## The one rule that no compiler enforces

`docs/DATABASE.md` is a contract. Three implementations write the same schema.
**Change an invariant in all three targets or in none** — a divergence nobody
recorded makes a report spanning several servers quietly wrong. Divergences that
are unavoidable go in that document's "Differences between targets" section.
