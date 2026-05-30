# Running a fresh-eyes review pass — verify renders, don't trust them

A "fresh-eyes" pass (landing on the GitHub repo as a stranger would) is the last
line of defence against the *repo-doesn't-match-the-cover-letter* failure mode
before an upstream submission. It only works if the reviewer reads **current**
state. Fetched GitHub HTML often isn't current — and a reviewer that reports
findings from a stale render turns the safety check into a noise generator.

This happened three times across the v1.x arc: a fresh-eyes pass reported a
personal email back in the README, the "Latest" release reverted to an old tag,
and repo topics missing. **All three were GitHub cache artifacts** — the repo
was clean and current; only the fetched render was stale. Acting on them would
have "fixed" non-problems and missed real drift elsewhere.

## The mechanism

GitHub serves different cache layers for different URLs:

- The **repository root** (`github.com/owner/repo`) renders the README inline
  into a composite page (README + About + Releases + Languages sidebar). That
  composite render is cached **aggressively** — the same `meta-request-id` can
  come back across fetches hours apart, on low-traffic repos especially.
- The **blob URL** (`github.com/owner/repo/blob/<branch>/README.md`) goes
  through a *different* rendering path with a *different* cache, and is far more
  likely to be current.

So a reviewer can read a months-stale front page from the root URL while the
actual default branch is many commits ahead.

## The workaround (apply from pass #1, not after three false alarms)

1. **Read the blob URL, not the root URL.** Fetch
   `github.com/owner/repo/blob/<branch>/README.md` for file content; the root
   URL only for sidebar metadata you then re-verify.
2. **Cross-check `meta-request-id` across fetches.** Same id on two fetches of
   the same URL = response-level caching. Compare a different endpoint (root vs
   blob) to confirm you broke out of the cache.
3. **Lead with verification, not findings.** Open with "I fetched X, here's what
   I see — *verify against the live repo before acting*; fetched HTML can be
   stale through opaque caching." Never lead with "found a regression" from
   fetched HTML alone.
4. **Treat every render-dependent finding as a hypothesis,** not a fact. The
   implementer confirms against ground truth before remediating.
5. **Trust direct observation over fetched HTML.** A local `git show
   <branch>:file`, `gh release list`, `gh repo view --json …`, or a user's
   hard-refreshed browser is ground truth; a fetched page is a derived view
   through unverified caches.

## The implementer side

The contributor receiving fresh-eyes findings runs the same measure-first loop
the rest of the project does: **verify each finding against the source of truth
(git, the `gh` API) before acting, and separate real drift from cache illusion.**
Cost: ~5 minutes per pass. Benefit: real findings get fixed, spurious ones get
discarded with confidence, and a scary-looking review resolves to its few
genuine items.

This is the project's "[a wall is a hypothesis](methodology.md)" discipline
applied to the *review process itself*: a stale render declaring "the README
regressed" is a wall; "is this read fresh?" dissolves it. See
[`methodology.md`](methodology.md) for the codebase-facing version of the same
loop.
