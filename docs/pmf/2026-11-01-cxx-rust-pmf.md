# PMF thesis: EmitRust / emitrust-bx (automated C++ -> safe Rust)

Date: 2026-11-01. Analyst draft. Every factual claim carries a URL; "[UNVERIFIED]" marks claims I could not confirm from a primary source.
Note on sources: some 2026 URLs below were found via web search and fetched once; summaries were produced by a fetch tool and may compress nuance.


## Executive PMF thesis

- **ICP:** the platform/security team inside a safety- or security-regulated C++ shop (defense primes and DoD labs, automotive/avionics Tier-1s, medical-device firms, and Microsoft/Google-style internal migration groups as design partners) that has to show *evidence* that a ported component behaves like the original, and cannot ship LLM output on faith.
- **Job to be done:** "Turn this self-contained C++ module (parsers, codecs, numeric kernels, config/protocol logic) into safe Rust I can sign off on, with a machine-checkable equivalence artifact, and tell me exactly which lines can't be done."
- **Wedge:** the *verifier and certifier for the translation*, not the translator. Every competitor, LLM pipelines included, is bounded by test-based equivalence (MIT LL's words). EmitRust's bijection, byte-diff in both directions, zero `unsafe` and located rejections make up the one artifact nobody else produces. Ship it first as a deterministic "safe core" translator for the canonical subset, plus an **LLM-proposes / bijection-checks** loop (FR-254) that pulls arbitrary C++ into that subset.
- **Why now:** the LLM generators are being sold directly (Gemini 4 Argon, 2026-09). TRACTOR Round 1 (2026-10-13) found that LLM pipelines are idiomatic but less correct, that static tools are correct but C-like, that failures are mostly late semantic mismatches, and that no performer reaches zero unsafe. Buyers now need a trust layer more than another generator.
- **Why us:** the only toolchain combining (a) zero `unsafe`, runtime included (Cpp2Rust has 1, TRACTOR's best performer averages 1.55 unsafe ops per test); (b) round-trip laws with byte-diff in both directions; (c) located rejections, the "surface errors, don't mask them" behaviour TRACTOR rewards. **Caveat:** none of this matters until the subset covers the vector/string/class/closure C++ that real modules are made of, and today it does not.

## Raw evidence log (written incrementally; synthesized below)

### Q1 Demand
- DARPA TRACTOR: goal is automated translation of large C codebases to memory-safe Rust, at "the same quality and style that a skilled Rust developer would produce". https://www.darpa.mil/research/programs/translating-all-c-to-rust . Solicitation DARPA-PS-24-20 (2024), two TAs. https://militaryaerospace.com/trusted-computing/article/55138748/software-c-to-rust-translating
- MIT Lincoln Lab is T&E; batteries released ~every 6 months; milestone projects 0 (Perlin noise) and 1 (SPHINCS+); Round 1 Evaluation Report linked. Performers NOT named on the page. https://www.ll.mit.edu/tractor
- TRACTOR benchmark paper (Okhravi et al., arXiv 2609.25121, 2026-09-20): metrics = correctness, safety, performance, idiomaticity. Benchmark targets C; C++ only mentioned as background. https://arxiv.org/abs/2609.25121
- ORBIT paper reports per-case "x of 6 performers" succeeded -> implies six TRACTOR performers evaluated. https://arxiv.org/pdf/2604.12048 [count from search snippet; verify]
- CISA/FBI "Product Security Bad Practices" (Oct 2024): no published memory-safety roadmap by 2026-01-01 for existing memory-unsafe products is "dangerous"; NON-BINDING; exempts products with EoS before 2030-01-01. https://www.ic3.gov/CSA/2024/241016-2.pdf ; https://techrepublic.com/article/cisa-fbi-memory-safety-recommendations
- Microsoft: Galen Hunt (Distinguished Engineer), Dec 2025 LinkedIn: "eliminate every line of C and C++ from Microsoft by 2030"; "combine AI _and_ Algorithms"; North Star "1 engineer, 1 month, 1 million lines of code". Clarified: "Windows is *NOT* being rewritten in Rust with AI"; "research project". https://www.theregister.com/2025/12/24/microsoft_rust_codebase_migration . KEY SIGNAL: the largest internal buyer is building in-house and explicitly hybrid (AI+algorithms) and explicitly C++, not just C.

### Q2 Competitors (C++)
- Cpp2Rust (PLDI 2026): "Automatic Translation of C++ to Safe Rust". https://pldi26.sigplan.org/details/pldi-2026-papers/23/Cpp2Rust-Automatic-Translation-of-C-to-Safe-Rust
- Cp2SRust (ISEC 2026), transpiler C/C++ to "safer" Rust. https://conf.researchr.org/details/isec-2026/isec-2026-research-papers/5/Cp2SRust-A-Transpiler-for-C-C-to-Safer-Rust
- LegacyToRustProject/cpp-to-rust: open-source agent, templates->generics+traits, differential run. https://github.com/LegacyToRustProject/cpp-to-rust
- RustRepoTrans benchmark (arXiv 2411.13990): 375 repo-level tasks into Rust from C++, Java, Python. https://arxiv.org/abs/2411.13990v5
- C-only code-quality study (arXiv 2602.00840): C2Rust, C2SaferRust, TranslationGym — none consistently match human translations. https://arxiv.org/html/2602.00840v1
- Cpp2Rust (PLDI 2026, INESC-ID/IST Lisbon + Google co-author): "first system" auto-translating C++ to functionally equivalent memory-safe Rust by INSERTING RUNTIME ownership/mutability checks; 2 libs, 13k LOC (WOFF2: 2% overhead; Brunsli: 6x slower due to pointer arithmetic). Not idiomatic by design. https://pldi26.sigplan.org/details/pldi-2026-papers/23/Cpp2Rust-Automatic-Translation-of-C-to-Safe-Rust
- ORBIT (Stony Brook, arXiv 2604.12048): agentic C->Rust; CRUST-Bench 24 programs: 100% compile, 91.7% test pass; unsafe 0.06-0.11% of lines vs C2Rust 69.6%; on 13 hardest TRACTOR cases 9 pass fully; "each case translated by 2 to 4 of six performers"; TRACTOR report: average performer pass rate 82.2% across Battery 01. https://arxiv.org/html/2604.12048
- Galois: GovTribe lists a $7.8M DARPA OT IDV (June 2025) for TRACTOR [secondary aggregator; UNVERIFIED vs DARPA]. https://govtribe.com/vendors/galois-inc-dot-1nct1 . UIUC "ForCLift" $5M per buildmvpfast blog [UNVERIFIED, low-quality source]. https://www.buildmvpfast.com/blog/ai-automated-rust-migration-cpp-memory-safety-rustify-2026
- IEEE Spectrum (2026-01-28): six TRACTOR teams funded, first-round results submitted in December; Great Refactor (Institute for Progress) proposes $100M FRO to convert 100M lines of OSS C/C++ by 2030; "current tools can reliably translate programs under 1,000 lines with little supervision, and up to 5,000 lines with oversight". Josh Triplett: "There will never be a silver bullet for AI being 100 percent robust against doing the wrong thing". https://spectrum.ieee.org/ai-code-rust-great-refactor
- Microsoft Copilot runtime port (The Register 2026-09-18): ~430k LOC TypeScript -> ~800k LOC Rust, ~$120k tokens + ~3 weeks dev time, 14.5 weeks, "a few dozen" regressions; Stephen Toub: "'if it compiles, it's correct' is useful only as a joke." (TS, not C++, but the best public cost datapoint for agentic rewrite.) https://www.theregister.com/devops/2026/09/18/microsoft-agentically-ports-copilot-runtime-to-rust-for-120k/5297549
- Google: "we do not need to throw away or rewrite all our existing memory-unsafe code" — vulnerabilities decay with code age; strategy = new code in memory-safe languages (2024-09-25). https://security.googleblog.com/2024/09/eliminating-memory-safety-vulnerabilities-Android.html . 2025: memory-safety bugs <20% of Android vulns; Rust net-new code ~= C++; 1000x lower memory-safety vuln density; ~4x lower rollback rate. https://blog.google/security/rust-in-android-move-fast-fix-things/
- Safe C++ (Baxter/Circle) deprioritized: SG23 vote 30/45 for profiles vs 20/45 for Safe C++; Baxter stopped work late 2024. https://theregister.com/2025/09/16/safe_c_proposal_ditched ; https://www.infoworld.com/article/4065702/safe-c-proposal-for-memory-safety-flames-out.html
- EU CRA: reporting duties from 2026-09-11; main obligations 2027-12-11; fines up to EUR15M/2.5% turnover; NO memory-safety-specific requirement found. https://www.jonesday.com/de/insights/2026/07/eu-cyber-resilience-act-24hour-reporting-duties-start-september-11-2026 ; https://finitestate.io/blog/cyber-resilience-act-timeline-2026-2027
- Industry case list (curated, secondary): Google "Argon" (Gemini 4) agents porting C/C++ incl. Fuchsia Zircon kernel (800k+ LOC) [verify]; Meta WhatsApp wamedia 160k LOC C++ -> 90k Rust, validated with differential fuzzing; Chrome jxl-rs replacing ~100k-LOC C++ decoder; ShiftCodex commercial AI migration platform. https://github.com/kevincouton/awesome-rust-migrations
- Cpp2Rust details (full paper read, https://web.ist.utl.pt/nuno.lopes/pubs/cpp2rust-pldi26.pdf ; code https://github.com/Cpp2Rust/cpp2rust): every variable becomes `Value<T>` = `Rc<RefCell<T>>`; authors: "The translated code is not very idiomatic"; supports functions, lambdas (by-value and by-ref capture), enums, pointer arithmetic, abstract classes, pure-virtual inheritance, virtual dispatch, templates via full instantiation, unique_ptr, std::vector/map/string/deque/pair/iterators. UNSUPPORTED: unions, goto, EXCEPTIONS, bitfields, placement new, user-defined copy/move ctors, dynamic_cast, const_cast, bases with fields or non-virtual methods, multiple inheritance, threads. Runtime library has ONE `unsafe` (Ptr::delete via Rc::from_raw). std::string -> Vec<u8>. => This is the closest academic competitor; it trades idiom + perf for coverage. EmitRust's differentiators vs Cpp2Rust: zero unsafe incl. runtime, idiomatic owned types, byte-diff validation, exceptions->Result.
- Google Gemini 4 "Argon" (2026-09-30): internal agentic migration of re2, libgav1 ("tens of thousands of lines"), Fuchsia Zircon kernel (800K+ lines); libgav1: 32K lines SIMD replaced with safe Rust, 2.7x faster than existing Rust port, identical output; verification = "rigorous automated and manual auditing, emulation testing, and review". Model to be offered via paid API ($2/$10 per M tokens intro). https://blog.google/innovation-and-ai/models-and-research/gemini-models/gemini-4-argon/  => frontier labs are selling the LLM-rewrite capability directly; a deterministic tool cannot compete on breadth.
- Meta WhatsApp wamedia: 160k LOC C++ -> 90k LOC Rust, built in parallel; validated by DIFFERENTIAL FUZZING + tests; pain points: binary size, multi-platform build support. Method (manual vs automated) not stated. https://engineering.fb.com/2026/01/27/security/rust-at-scale-security-whatsapp/
- Crubit (C++<->Rust bindings): cpp_api_from_rust available in Chromium July 2026, "experimental and unstable"; rust_api_from_cpp no Chromium support yet; hard to deploy outside Google monorepo; no direct calls for fns taking std::string. https://groups.google.com/a/chromium.org/g/rust-dev/c/Kixvw8bhfME ; https://github.com/google/crubit
- Ferrocene: ISO 26262 ASIL D / IEC 61508 SIL 3 / IEC 62304 Class C qualified toolchain; core-library subset certified SIL 2 (Dec 2025) and ASIL B (Feb 2026). https://www.businesswire.com/news/home/20251203818102/en ; https://www.eenewseurope.com/en/ferrocene-26-02-0-automotive-core-certification . No automaker production adoption confirmed [evidence thin].
- CISA/NSA CSI "Memory Safe Languages: Reducing Vulnerabilities..." (2025-06-24): existing code need not be entirely rewritten; recommends interop + incremental. https://www.nsa.gov/Press-Room/Press-Releases-Statements/Press-Release-View/Article/4223298/ ; https://cisa.gov/resources-tools/resources/memory-safe-languages-reducing-vulnerabilities-modern-software-development
- ONCD "Back to the Building Blocks" (2024-02-26): urges MSLs; guidance not regulation. https://www.techrepublic.com/article/white-house-report-memory-safe-programming-languages/
- "Challenges in C++ to Rust Translation with LLMs" (ACM 2025, file-level, GPT-4-turbo/DeepSeek-v3): classes and templates hardest; template accuracy ~47.6% (DeepSeek-v3) / 42.9% (GPT-4-turbo) [numbers from search snippet; PDF 403 — UNVERIFIED in full]. https://dl.acm.org/doi/10.1145/3759425.3763380
- RustRepoTrans: compilation errors = 92.3% of failures for repo-level translation into Rust. https://arxiv.org/pdf/2411.13990 [snippet]

### PRIMARY: MIT LL "First Test & Evaluation Report for TRACTOR C to Rust Translators" (Okhravi et al., 2026-10-13)
URL: https://github.com/DARPA-TRACTOR-Program/Reports/blob/main/First_TRACTOR_Evaluation_Report.pdf (linked from https://www.ll.mit.edu/r-d/projects/translating-all-c-rust-tractor-benchmarks ). Read in full text locally.
- Six performer teams: Aarno Labs+MIT (Tenjin: deterministic, C2Rust-based, staged unsafe elimination, no LLM in automated mode); Galois+Immunant+UPenn+UT Austin (CRISP: deterministic transpile + bounded LLM safety rewriting); Intel+UC Berkeley (IDEAS: snippet-level LLM, strict no-unsafe, separate ABI wrappers); U Washington+Better Bytes (project-level LLM + Hayroll macro preservation); U Wisconsin+UIUC+Edinburgh (whole-project LLM-forward); Yale+Northeastern (PROCTOR: static-analysis Rust-to-Rust refinement, borrow inference).
- LLM use must be "architecturally pluggable" (model-vendor independent).
- Battery 01 = 150 tests, scoped to "well-defined pointer usage, simple data structures, conventional error handling, and static program structure, while avoiding dynamic allocation and complex aliasing". Best correctness: Yale 98.7% (148/150), Aarno 95.3%; avg failure 17.8%.
- Safety: best Intel 1.55 avg unsafe ops/test; per-performer share of tests with ZERO unsafe ops (excluding required C FFI) ranged 15.2%-66.9% (Fig. 12). NO performer is at zero unsafe across the battery.
- Idiomaticity: Intel best (2.67/2.89/2.67 of 3). Milestones: 4 teams 100% (40/40) functionality.
- Key finding (verbatim): "static analysis-based approaches tend to achieve higher functional correctness, whereas LLM-based approaches produce more idiomatic Rust code ... hybrid approaches combining the strengths of both techniques may ultimately offer the most effective overall solution."
- "Practical correctness is bounded by test-based equivalence rather than more formal notions of equivalence ... We encourage performers to develop more sophisticated notions of equivalence".
- "the majority of failures occur at the comparison stage rather than during translation or compilation" — i.e., semantic drift, not build errors, is the 2026 bottleneck.
- "Silent semantic patching is more dangerous than preserving UB"; "The evaluation implicitly favors approaches that surface errors over those that mask them".
- "Both naive C2Rust and naive LLM baselines perform better than expected on the constrained first battery" — early batteries underestimate difficulty.
- C++ is out of scope for TRACTOR (C only).

### More evidence
- Google giflib (InfoQ 2026-09): ~3,000 LOC C ported by single-shot Gemini prompt + manual fix of unsound raw-pointer handling; validated by differential fuzzing ~200M iterations over six days and 30M real GIFs bit-for-bit; deployed, sandbox retired (p99 latency improved); authors: AI translation "not a hands-off panacea"; community: deterministic transpiler + AI refactor "may scale better". https://infoq.com/news/2026/09/c-rust-rewrite/  => The VALIDATION harness (differential), not the generator, is what made it shippable — direct support for EmitRust's byte-diff positioning.
- C++26 (finalized March 2026): hardened stdlib (P3471; Google reports >1,000 bugs fixed, ~30% fewer segfaults, ~0.3% perf), erroneous behaviour for uninit locals (P2795), contracts. Profiles slipped to C++29; Safe C++ (P3390) discontinued in ISO. https://www.wholetomato.com/blog/c26-memory-safety-what-actually-ships-for-existing-code/ ; profiles plan P4186R0 https://open-std.org/JTC1/SC22/WG21/docs/papers/2026/p4186r0.html  => C++ incumbents get "cheap" partial safety without migration, weakening urgency for some buyers.
- Clang Lifetime Safety (Google/Apple): Polonius-inspired intra-procedural dangling-pointer analysis, `-Wlifetime-safety-permissive`; explicitly "not a borrow checker"; future: Rust-style annotations, incremental borrow-checker rules. https://clang.llvm.org/docs/LifetimeSafety.html ; https://discourse.llvm.org/t/rfc-intra-procedural-lifetime-analysis-in-clang/86291 ; https://llvm.org/devmtg/2025-10/slides/technical_talks/saxena.pdf
- SaferCPlusPlus/scpptool: enforced safe C++ subset via replacement types; flags std containers as unsafe; no evidence of standard adoption. https://github.com/duneroadrunner/scpptool
- Rust Foundation Interop Initiative ($1M from Google, Feb 2024): problem statement names C-ABI-only boundaries, unsafe FFI, and build-system integration as core pains. https://rustfoundation.org/media/rust-foundation-interop-initiative-update-from-research-to-implementation/
- Exceptions: 2,721 GitHub C++ systems studied (Bradley & Godfrey SCAM 2019). https://plg2.cs.uwaterloo.ca/~migod/papers/2019/scam19.pdf ; "52% report exceptions banned in part/all of code" (ISO C++ 2018 survey via Reddit) [UNVERIFIED]. No published mining study of STL container frequency found [EVIDENCE GAP]; vector-dominance is folklore (e.g. https://hjemmesider.diku.dk/~jyrki/Myris/KS2009bC.html).
- Templates: TOSEM 2020 study, 50 systems/566M LOC over revisions, template use follows a power law. https://unpaywall.org/10.1145%2F3356579 [snippet-level]
- VERT (ASE 2025): LLM + Wasm-derived oracle; on 1,394 competitive-programming programs, PBT pass 31%->54%, bounded MC 1%->42% vs Claude alone; struggles with pointers/multi-function. https://arxiv.org/pdf/2404.18852
- SafeTrans claims C2SaferRust, VERT, FLOURINE, SACTOR fail to compile or produce extensive unsafe on its libs [competitor claim]. https://arxiv.org/pdf/2505.10708
- Safety-qualified Rust compilers for automotive MCUs (HighTec ASIL D, Infineon AURIX, Renesas RH850); dominant pattern is HYBRID C + Rust in AUTOSAR, not translation. https://hightec-rt.com/news/blog/item/htc-intellias ; https://www.eejournal.com/industry_news/hightec-drives-automotive-software-development-on-renesas-rh850-u2x-mcus-with-llvm-based-rust-and-c-c-toolchains/
- Commercial vendors: no public per-line pricing found; ShiftCodex (AI translate-test-fix loop) exists https://shiftcodex.com/ ; one analyst forecasts commercial third-party AI translation products "likely in the 2027-2028 window" https://www.softwareseni.com/ai-assisted-code-translation-from-c-to-rust-how-it-works-and-when-it-will-be-available/ [analyst opinion]. JetBrains ran a June 2026 livestream "How do you rewrite C/C++ projects to Rust?" https://info.jetbrains.com/rustrover-livestream-june23-2026.html (signal of developer interest).

---------------------------------------------------------------------------------------------------

## Q1. Demand: who pays or mandates, and how hard

| Source of demand | What is real (cited above) | Binding? | Money | Implication for us |
|---|---|---|---|---|
| DARPA TRACTOR | 6 teams (Aarno, Galois+Immunant, Intel, UW, Wisconsin, Yale); batteries every ~6 months; Round 1 report 2026-10-13 | Program, not mandate | Galois IDV $7.8M [secondary, UNVERIFIED]; other awards not public | **C only.** It rewards correctness, zero unsafe, idiomaticity and surfacing errors. It is a scoreboard and a credibility source, not a C++ market. |
| US gov guidance (ONCD 2024, CISA/FBI 2024, CISA/NSA 2025) | Asked for a memory-safety ROADMAP by 2026-01-01; explicitly says rewriting everything is not required; favours interop and incremental adoption | **Non-binding** | none direct | It creates a *roadmap* line item, not a translation budget. Buyers can satisfy it with "new code in Rust + hardening". |
| EU CRA | Reporting from 2026-09-11; essential requirements 2027-12-11; fines up to €15M / 2.5% | Binding | indirect | No memory-safety-specific clause found. It is a general security-liability pressure, not a Rust pull. |
| Microsoft | Hunt: "eliminate every line of C and C++ ... by 2030", "AI *and* Algorithms", "1 engineer, 1 month, 1 million lines"; later called a "research project"; Copilot runtime agentic TS->Rust port: $120k tokens + dozens of regressions | Internal | large, in-house | The biggest buyer is building its own tooling and explicitly wants hybrid algorithmic + AI. Best fit is a design-partner or acqui-hire path, not a licence sale. |
| Google | Policy is "we do not need to ... rewrite all our existing memory-unsafe code" (vulnerabilities decay with age); yet Argon ports re2/libgav1/Zircon and giflib; Crubit for interop; co-author on Cpp2Rust | Internal | in-house | Google rewrites **selectively**: untrusted-input parsers and codecs. That is exactly the "self-contained module" wedge. Google sells the generator (Argon API), not the verifier. |
| Meta/WhatsApp | wamedia, 160k C++ to 90k Rust, differential fuzzing | Internal | in-house | Validation is by differential fuzzing. The pain points were binary size and build. |
| Automotive / avionics / medical | Qualified Rust compilers exist (Ferrocene ASIL D/SIL 3/62304 Class C; HighTec ASIL D); core-lib subset ASIL B. Dominant pattern is hybrid C + Rust in AUTOSAR | Standards require tool confidence | budgets exist for qualified tools | **Evidence is thin** that anyone is translating safety code automatically. Certification favours deterministic, explainable translation rules (TQL-1 code generators are "correctness by construction"), which fits a bijection far better than an LLM. No buyer quotes found. |
| Great Refactor (IFP) | Proposed $100M FRO; 100M lines of OSS by 2030; "build tools to validate the security and functionality of AI-translated code" | Proposal | not funded [as of source] | A named demand for **validation tooling**, our wedge, if it gets funded. |

**Verdict on demand:** urgency is real but fragmented. Deadline pressure is weak (guidance is non-binding, and C++26 hardening plus profiles give C++ shops a stay-in-C++ option). Concentrated budgets sit with (1) hyperscalers who build in-house and (2) DoD-adjacent R&D. Merchant-market budgets for C++→Rust tooling are **unproven**: I found no vendor with public pricing and no funded C++→Rust startup round [evidence gap].

## Q2. Competitive landscape

| Tool | Input | `unsafe` in output | Correctness guarantee | Idiomatic? | Availability / traction |
|---|---|---|---|---|---|
| C2Rust (Immunant/Galois) | C | ~all unsafe (ORBIT: 69.6% of lines) | syntax-directed, compiles | No | OSS; basis of Aarno Tenjin and Galois CRISP |
| TRACTOR performers (6) | C | best avg 1.55 unsafe ops/test; zero-unsafe share of tests 15–67% | test vectors; best 98.7% on B01 | LLM teams more idiomatic | Research; some OSS (Hayroll etc.) |
| ORBIT (Stony Brook) | C | 0.06–0.11% of lines | tests; 91.7% CRUST-Bench | yes (LLM) | Academic |
| VERT | C/Go→Rust | low | Wasm oracle + PBT / bounded MC (42% BMC) | yes | Academic, small programs |
| SACTOR / SafeTrans / RustMap / EvoC2Rust / Rustine / C2SaferRust | C | varies | tests | partial | Academic |
| **Cpp2Rust (PLDI'26, IST + Google)** | **C++** | 0 in output, **1 in runtime** | "functionally equivalent" by construction with runtime borrow checks; evaluated on 13k LOC | **No**: every variable is `Rc<RefCell<T>>`; strings become `Vec<u8>` | OSS (github.com/Cpp2Rust). Covers classes, virtual dispatch, templates, lambdas, vector/map/string/deque. **No exceptions, no multiple inheritance, no user copy/move ctors** |
| Cp2SRust (ISEC'26) | C/C++ | "safer" | ? | ? | Academic [details unverified] |
| LLM agents (Gemini 4 Argon, Copilot/Claude-based internal efforts, ShiftCodex, LegacyToRust cpp-to-rust) | C/C++ | low-ish | tests / differential fuzzing / manual audit | **Yes** | Argon going to paid API; Microsoft internal; startups early, no public traction found |
| Interop (cxx, autocxx, Crubit) | C++ ↔ Rust | unsafe at boundary | n/a | n/a | Crubit "experimental" in Chromium (Jul 2026); cxx is the default |
| **EmitRust / bx** | C + C++17 subset | **0, runtime included** | byte-diff stdout vs clang; round-trip laws both directions; located rejection | structured (match, struct+impl, Drop, Result) | Pre-commercial; TRACTOR baseline 55/252 cases [internal memory note, not public] |

**The positioning gap:** among C++ tools, Cpp2Rust buys coverage with non-idiomatic `Rc<RefCell>` output and runtime panics. LLMs buy idiom with unverifiable semantics. Nobody offers *idiomatic and certified-equivalent on a declared subset*. That cell is empty, and it is empty because it is hard, not because nobody wants it.

## Q3. What buyers say they need (primary quotes)

1. **Verifiable correctness beyond "it compiles"** is the dominant theme.
   - Toub (Microsoft): "'if it compiles, it's correct' is useful only as a joke." (The Register, 2026-09-18)
   - MIT LL: "the majority of failures occur at the comparison stage"; "Practical correctness is bounded by test-based equivalence ... We encourage performers to develop more sophisticated notions of equivalence." (TRACTOR R1)
   - Triplett: "There will never be a silver bullet for AI being 100 percent robust against doing the wrong thing." (IEEE Spectrum)
   - Every shipped rewrite used differential validation: giflib (200M differential fuzz iterations), WhatsApp (differential fuzzing), libgav1 ("identical video output").
2. **No silent semantic change.** MIT LL: "Silent semantic patching is more dangerous than preserving UB"; evaluators favour "approaches that surface errors over those that mask them". This is EmitRust's located-rejection contract, word for word.
3. **Idiomatic and maintainable output.** DARPA: "the same quality and style that a skilled Rust developer would produce". Wallach: "does the code look the way a skilled Rust programmer would have built it from scratch".
4. **Incremental adoption and interop.** CISA/NSA say existing code need not be rewritten entirely. The Rust Foundation names C-ABI-only boundaries and build integration as the core pains. WhatsApp's pains were build-system support and binary size. Every automotive example is hybrid.
5. **Cost and throughput.** Microsoft's North Star is "1 engineer, 1 month, 1 million lines". The $120k token bill was considered cheap, and the regressions, not the tokens, were the cost.
6. **Certification evidence.** This is inferred, not quoted. DO-330 TQL-1 code generators are qualified as "correctness by construction", with explicit translation rules. I found **no** buyer quote asking for a certifiable C++→Rust translator [evidence gap; must be validated by customer interviews].

## Q4. The C++ gap

- **Measured data is scarce** [evidence gap]. No public mining study ranks STL container usage. Templates follow a power law across 566M LOC (TOSEM 2020). Exceptions are studied across 2,721 GitHub projects (SCAM 2019), and a large share of industrial C++ bans or limits exceptions (52% figure UNVERIFIED; Google style forbids them [not fetched]).
- **What is hard for LLMs:** file-level GPT-4-turbo/DeepSeek-v3 struggle most with classes and templates (~43–48% template accuracy) [ACM 2025, snippet-level]. In repo-level translation, 92.3% of failures are compile errors (RustRepoTrans).
- **What is hard for rule-based tools:** Cpp2Rust's unsupported list is the honest map. It has no exceptions, multiple inheritance, bases with fields or non-virtual methods, user-defined copy/move ctors, dynamic_cast, unions, bitfields or threads. Aliasing forces `Rc<RefCell>` everywhere, which costs 6x on pointer-arithmetic-heavy code.
- **Practical ranking for a subset translator,** inferred from these sources plus the folklore that vector/string dominate:
  1. vector/string/array and range-for
  2. classes with value semantics, ctors/dtors, and copy-vs-move
  3. unique_ptr/optional
  4. lambdas and `<algorithm>`
  5. map/unordered_map
  6. single inheritance with virtuals mapped to traits
  7. templates by instantiation
  8. exceptions mapped to Result (only where not banned)
  9. shared_ptr cycles, multiple inheritance, iterator invalidation and aliasing (hardest; reject)

## Q5. Where "provable bijection + byte-diff + zero unsafe, on a subset" wins and loses

**It wins:**
- When the buyer must *prove* equivalence or produce audit evidence (defense, safety-critical, crypto/codecs on untrusted input). A deterministic, rule-explainable mapping is the only kind with a plausible tool-qualification story (TQL-1 analogy).
- On **reverse direction and round-trip**. No competitor offers Rust→C++. This enables "keep a C++ build of the Rust-maintained code" during transition, and a one-to-one review diff a C++ reviewer can read.
- On cost determinism (no tokens, reproducible), and wherever LLM regressions are expensive (Copilot port: dozens).
- As a **checker for LLM output**. It is the "more sophisticated notion of equivalence" TRACTOR explicitly asks for, provided the LLM output lands inside the subset.

**It loses:**
- On breadth. Real modules use the features the subset lacks. LLM agents already port 800k-LOC kernels (Argon) and 430k-LOC apps at $120k. A subset tool that rejects 80% of functions is a linter, not a migrator.
- On idiom beyond the canonical form. Bijection means the Rust side must mirror C++ structure, so it cannot do LLM-style restructuring into iterators and traits.
- On the market-access front: hyperscalers build in-house, and TRACTOR is C-only.

**The hybrid wedge ("LLM/normalizer rewrites into the subset; the bijection certifies the rest") is credible, and it is the strongest framing.** It matches TRACTOR's core finding: static analysis is correct, LLMs are idiomatic, so hybrids win. It also matches Hunt's "AI *and* Algorithms". The risk is that the subset must be big enough that "normalize into it" is usually possible. Otherwise the normalizer is itself a full C++→C++ LLM rewrite with all the same trust problems, merely moved to the C++ side. There it is checked by native-vs-native byte-diff, which is still a real improvement, because C++→C++ behaviour checks are cheap.

**bc.hpp (borrow-checked C++ dialect) as an on-ramp is weak demand, strong demo.**
- Safe C++ (the most complete version of this idea) was rejected by WG21 (30/45 for profiles; discontinued).
- Profiles slipped to C++29.
- Clang's lifetime analysis is explicitly "not a borrow checker".
- scpptool exists with no visible adoption.
- Writing new code in a C++ dialect that maps 1:1 to Rust is a hard sell when the same team could write Rust. It only makes sense for teams forbidden from adopting Rust toolchains (certification lag), and Ferrocene/HighTec are removing that barrier.
- **Use bc.hpp as an internal normal form and a teaching artifact, not a product.**

## Top 3 candidate wedges

### W1. "Certified safe core" for untrusted-input C++ modules (codecs, parsers, format libraries). Recommended primary.
- **Target user:** security and platform engineers porting a self-contained, input-facing C++ library. These are exactly the components Google, Meta and Chrome chose: giflib, libgav1, wamedia, jxl.
- **Must-have capability:**
  - Translate the module's core functions to zero-`unsafe`, owned-type Rust (no `Rc<RefCell>` everywhere).
  - Prove behaviour on the module's own inputs (byte-diff).
  - Emit a located list of everything left over, plus cxx/C-ABI glue so the remainder keeps working.
- **Leading indicator (weekly):** % of functions in **Cpp2Rust's own evaluation programs, WOFF2 and Brunsli (13k LOC; https://github.com/Cpp2Rust/cpp2rust)**, translated with 0 unsafe and byte-identical output on their test inputs. Report it next to Cpp2Rust's number and `Rc<RefCell>` count. Secondary: the same metric on one Google-ported library (e.g. re2 subset).
- **Main risk:** these modules lean on pointer arithmetic and byte buffers (Brunsli was 6x slower even for Cpp2Rust). The subset may cover too little for months, and LLM ports (giflib-style) may be "good enough" for buyers.

### W2. Equivalence checker / normalizer for LLM-driven C++ migration ("AI proposes, bijection certifies")
- **Target user:** internal migration teams already running LLM agents (Microsoft's FSSE group, Google Argon users, TRACTOR performers, ShiftCodex-style vendors) who need a stronger notion of equivalence than tests, which TRACTOR explicitly calls the current ceiling.
- **Must-have capability:** FR-254. An LLM or rule rewrite of arbitrary C++ into canonical subset C++, validated native-vs-native, after which the bijection gives Rust with a round-trip certificate. Exposed as a CLI/CI step that returns a pass, a located reject, or a diff.
- **Leading indicator (weekly):** % of functions in a fixed public C++ corpus that end inside the certified subset **after normalization**, against before. Candidates are this repo's llvm-test-suite C++ slice, or RustRepoTrans's C++ tasks (arXiv 2411.13990). Track rejected-by-checker LLM rewrites as a second number (proof the checker catches real bugs).
- **Main risk:** the normalizer is itself an LLM rewrite. If subset coverage is small, almost nothing normalizes, and the value collapses into "native differential testing", which buyers can already do with fuzzing.

### W3. Assured C→safe-Rust for DoD/TRACTOR-adjacent programs (the existing C path)
- **Target user:** DoD labs, primes and TRACTOR transition partners that need zero-`unsafe` output and visible rejection. On those two axes Round 1 shows every performer falling short of zero unsafe.
- **Must-have capability:** TRACTOR strict-mode PASS on the public batteries with 0 unsafe, plus the PORTING.md and progress JSON as the transition artifact.
- **Leading indicator (weekly):** TRACTOR PASS / 252 cases (currently 55, per internal measurement), reported **per distinct program (125)** as the repo's counting rules require, together with unsafe = 0.
- **Main risk:** it is not C++, so it does not use bx. Six funded teams already score 82–99% on B01. Competing on PASS against teams with LLM breadth is uphill, and the differentiator (zero unsafe) is necessary but may not be decisive. Treat it as the credibility channel, not the business.

## Roadmap implications, ranked by PMF impact

1. **FR-251 vector + string.** Gate item. No real C++ module survives without them, and the evidence (folklore, unmeasured) is that vector/string dominate container use. Cheapest high-coverage step.
2. **NEW: classes/structs with value semantics in bx** (ctors/dtors to Drop, copy vs move via ctorType, member functions to impl, `unique_ptr`/`optional`). The main MLIR importer already does struct+impl and Drop, but bx does not. Without classes, coverage on any real module is near zero. Pick ONE product path (bx or lib/ImportC) for C++ before investing in both [internal observation].
3. **NEW: public head-to-head benchmark plus a function-level frontier report** on WOFF2/Brunsli and one more Google-ported library. It turns the leading indicator into a weekly number, gives sales a single chart against Cpp2Rust and LLMs, and costs little.
4. **FR-254 normalizer search.** This is the wedge engine and what makes the subset matter for arbitrary code. Start with LLM-proposed rewrites validated by native-vs-native byte-diff, not exhaustive search. Ranked after 1–3 because a normalizer into a tiny subset yields ~0.
5. **NEW: interop boundary generation** (cxx/C-ABI shims for rejected items so a partial port links into the remaining C++ build). Every buyer source says incremental beats big bang, and without this a partial port is unusable in a C++ product. Must satisfy the zero-unsafe rule: use `cxx`'s safe bridge and say plainly where the unsafe lives.
6. **FR-252 closures.** High frequency in modern C++ and needed for `<algorithm>` comparators, but second-order to data types.
7. **FR-253 `<algorithm>`.** Real value, already de-risked (the libstdc++ introsort port found a real miscompile, a good sales anecdote about why LLM `sort_unstable_by` is wrong). It follows closures.
8. **NEW: std::map/unordered_map + single inheritance to traits.** Next coverage tier, on Cpp2Rust's supported list.
9. **NEW: exceptions to Result in bx.** Present in the main importer. Lower priority because many industrial codebases ban exceptions (UNVERIFIED share), and Cpp2Rust doesn't support them either, so it is a differentiator, not a gate.
10. **NEW: multi-file projects + CI integration** (GitHub Action producing the equivalence certificate and PORTING.md diff per PR). This is the adoption surface for W2. Do it once there is something worth gating.
11. **FR-255 bc.hpp.** Lowest. WG21 rejected Safe C++, profiles slipped to C++29, and Clang disclaims borrow checking. Nobody is asking for a new C++ dialect. Keep it as an internal canonical form and research artifact; don't productise.

## Kill criteria (no PMF if...)

1. **Coverage ceiling:** after FR-251 + classes + FR-252/253 (≈1 quarter), fewer than ~30% of functions in WOFF2, Brunsli and one other named real module translate fully (0 unsafe, byte-identical), and normalization (FR-254) cannot lift that above ~50%. The subset is then a toy relative to Cpp2Rust's coverage.
2. **No buyer pull for evidence:** in 10–15 interviews with ICP teams (defense labs, automotive/avionics Tier-1s, hyperscaler migration groups), fewer than 3 say equivalence evidence *beyond* differential fuzzing plus tests would change a decision or a budget. The giflib/WhatsApp precedent suggests fuzzing may already be "good enough".
3. **LLM parity:** frontier agents (Argon-class) produce byte-identical, zero-unsafe ports of the same benchmark modules, at a token cost under ~1 engineer-week, with regression rates buyers accept without an extra checker.
4. **No design partner** (a TRACTOR transition customer, a Tier-1, or an internal hyperscaler team) commits to a pilot on their own code within two quarters.
5. **Idiom rejection:** reviewers or Clippy-based scoring (TRACTOR-style 0–3 readability/syntax/types) rate bijection-canonical Rust clearly below LLM output, so the bijection's structure-mirroring is a liability, not a feature.
6. **Competitor closes the gap:** Cpp2Rust (with a Google co-author) or a TRACTOR team ships idiomatic, owned-type C++ translation with equivalent coverage and a formal equivalence story.

## Evidence quality notes
- Strong (primary, read): TRACTOR R1 report; Cpp2Rust paper; Register/Microsoft quotes; Google Argon blog; Meta WhatsApp blog; ORBIT paper; C++26 status article; Clang docs.
- Medium (reputable secondary): IEEE Spectrum, InfoQ giflib, CRA law-firm summaries, Safe C++ vote coverage.
- Weak or UNVERIFIED: Galois $7.8M figure (GovTribe); ForCLift $5M; the 52% exceptions-banned figure; the ACM C++-LLM template accuracy numbers (snippet only); C++ container frequency (no study exists that I found); whether any regulated-industry buyer wants an automated translator (no quotes found).
- Date caution: several sources are dated 2026 and were fetched via search tools that summarise. Spot-check before external citation.

## Steering log

### 2026-11-01 ~13:30 CST -- kill criterion 1 measured as not reachable by breadth

- **Measured.** Function-level EMIT on pinned WOFF2+Brunsli (392 functions, `docs/pmf/frontier/`)
  went 1 → 1 → 6 → 18 over FR-260, FR-257 (structs), FR-261 (pointers as slices) and FR-259
  (typedefs, `?:`, `x++`). That is **4.6%**. The FR-262 spike gives the ceiling for the next
  tier: making project headers visible adds +3..+6 strict EMIT; cross-TU linking adds +0. The
  census upper bound for everything the importer could reach is **28.3% raw, ~14% after the
  repo's halving rule**.
- **Reading.** Criterion 1 asks for ≥30% after classes plus FR-252/253, with ~50% reachable by
  normalization. The breadth path on these two modules does **not** reach it. This is not yet a
  kill. The criterion names a third module, and FR-254 (normalization) is unmeasured. But it
  stops further woff2/brunsli-specific breadth from being the default next step.
- **Decision.** Build the parts of the original request that were still open: closures
  (FR-252, landed 7f5477e) and `<algorithm>` (FR-253, in progress). Then FR-254's seam, which is
  the only lever on criterion 1 that is left. Each FR that lands adds a frontier data point
  only when it can move the number.
- **Found on the way (the verifier thesis).** FR-263 (73241e5): the subset accepted programs
  with **two legal C++ outputs**. g++ and clang print different bytes, and a clang-only
  byte-diff oracle could not see it. It was caught by an adversarial probe, not by the oracle.
  For the "sell the verifier" positioning, the rule needs **multiple compilers** in the
  oracle, not one.
- **Not actionable by the build loop:** criteria 2 and 4 (buyer interviews, a design partner).
  These are the user's to run. Nothing measured here substitutes for them.

### 2026-11-01 ~16:30 CST -- FR-253 landed; suites say +0; normalizer is the remaining lever

- **Landed.** FR-253 (cf39265): `<algorithm>`/`<numeric>` over whole vectors, both
  directions, zero `unsafe`, comparator sort as an exact libstdc++ introsort port. With
  FR-252 this closes the four areas of the original request (closures, algorithm, vector,
  string) at the Kernel level.
- **Measured, +0.** Side-by-side through ledger_runner with private manifests: Cpp17Suite
  bx 6/35 vs emitrust-cc 35/35; CppStdSuite bx 4/65 vs 11. Both bx sets are strict subsets
  of emitrust-cc's, and bx miscompiled 0. FR-254's pre-spike had already refuted "a small
  rule set" on woff2/brunsli (≤15 functions, ~7 honest).
- **Reading.** Breadth by more Kernel rows has stopped paying on every corpus measured.
  What bx has that emitrust-cc does not is the both-direction, two-compiler, zero-miscompile
  guarantee over its subset. That supports the verifier positioning (criterion 3), not the
  coverage one (criterion 1).
- **Decision.** Build FR-254's first rules (`for`→`while`, do-while) with their double
  validation, behind an opt-in `--normalize`. The purpose is to make the search seam real
  and measurable on the frontier (`bx-frontier --bx-arg=--normalize`), not to chase
  criterion 1. If the measured gain is ≤ the halved estimate, stop adding rules and record it.

### 2026-11-02 ~00:30 CST -- normalizer R1-R3: EMIT 18 -> 30 of 392 functions

Three sound rewrites (for->while, do-while, post-incdec) moved woff2/brunsli EMIT from 18 to
30 functions (4.6% -> 7.7%), 0 lost, each verified by byte-diff of the ORIGINAL against the
normalized C++ and the Rust. Per rule: +8, then +4. That is real movement, but kill
criterion 1 (30%) is still ~4x away, and the movers were the functions with a single blocker;
what remains carries multi-tag sets (member functions, library calls, constructors, private
members). The hybrid spike (an LLM rewrites blocked functions into the subset, certified by
differential execution of the extracted original under sanitizers) is the measurement that
decides whether the wedge closes that gap; it is blocked on a tool-permission decision, not
on design.
