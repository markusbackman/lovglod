# LövGlöd — instruktioner för Claude

## Commits och releaser

Innan du committar: fråga alltid om ändringen är avsedd för en **stabil release**
eller en **pre-release (beta)**. Gissa inte, och anta inte att svaret från en
tidigare commit gäller igen.

Svaret avgör versionstaggen, och taggen avgör vilka lampor som får firmwaren
(`.github/workflows/release.yml`):

- Stabil → `vX.Y.Z` — publiceras som vanlig release, når alla lampor.
- Pre-release → `vX.Y.Z-rcN` — publiceras som pre-release, når bara lampor
  med Uppdateringskanal: Beta.

Tagga och pusha bara när användaren ber om det. Se docs/BYGGA.md, "Betaprogrammet".
