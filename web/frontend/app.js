/* Playground client.
   ---------------------------------------------------------------------
   The one rule worth stating up front: this file never decides whether
   something is free. It asks the server and reacts. The cache/auth boundary
   lives entirely in /api/compile -- a client-side "are you signed in" check
   would be both wrong (a cached result needs no account) and forgeable.

   So the flow is always: POST the compile, and if the answer is 401, THEN
   show the sign-in wall. That means an anonymous visitor clicking a cached
   example never sees an auth prompt at all.
*/

(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);

  const el = {
    examples: $("examples"),
    source: $("source"),
    output: $("output"),
    diagnostics: $("diagnostics"),
    status: $("status"),
    lane: $("lane"),
    run: $("run"),
    meter: $("meter"),
    signout: $("signout"),
    gsi: $("gsi-button"),
    srcname: $("srcname"),
    srcmeta: $("srcmeta"),
    outname: $("outname"),
    filetabs: $("filetabs"),
  };

  const OPTION_IDS = [
    "emit", "language", "actor_mode",
    "preserve_c_names", "recover", "c_abi_exports",
    "incremental", "no_actor_lift",
  ];

  const state = {
    token: sessionStorage.getItem("emitrust_token") || null,
    examples: [],
    activeSlug: null,
    files: [],
    activeFile: null,
    busy: false,
  };

  /* ---------------------------------------------------------- options -- */

  function readOptions() {
    const o = {};
    for (const id of OPTION_IDS) {
      const node = $(id);
      if (!node) continue;
      o[id] = node.type === "checkbox" ? node.checked : node.value;
    }
    return o;
  }

  function writeOptions(options) {
    for (const id of OPTION_IDS) {
      const node = $(id);
      if (!node || !(id in options)) continue;
      if (node.type === "checkbox") node.checked = Boolean(options[id]);
      else node.value = options[id];
    }
    syncSourceName();
  }

  function syncSourceName() {
    el.srcname.textContent = $("language").value === "cpp" ? "input.cpp" : "input.c";
  }

  /* ------------------------------------------------------- rendering -- */

  const escapeHtml = (s) =>
    s.replace(/[&<>]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;" }[c]));

  /* Deliberately conservative highlighting: it runs on compiler output, so
     being wrong is worse than being plain. Strings and comments are matched
     first so keywords inside them are never recoloured. */
  const RUST_KEYWORDS =
    /\b(fn|let|mut|struct|impl|enum|match|if|else|for|while|loop|return|use|pub|const|static|unsafe|self|Self|as|in|move|ref|where|trait|dyn|crate|mod|type)\b/g;
  const C_KEYWORDS =
    /\b(int|char|void|float|double|long|short|unsigned|signed|struct|union|enum|static|const|extern|return|if|else|for|while|do|switch|case|break|continue|sizeof|typedef|_Bool|goto|default)\b/g;
  const TYPES =
    /\b(i8|i16|i32|i64|i128|isize|u8|u16|u32|u64|u128|usize|f32|f64|bool|str|String|Vec|Option|Box)\b/g;

  function highlight(text, kind) {
    const out = [];
    // Split on strings and comments so we never highlight inside them.
    const parts = text.split(/("(?:[^"\\]|\\.)*"|\/\/[^\n]*|\/\*[\s\S]*?\*\/|#[^\n]*)/g);
    for (let i = 0; i < parts.length; i++) {
      const part = parts[i];
      if (part === undefined || part === "") continue;
      if (i % 2 === 1) {
        const cls = part.startsWith('"') ? "tok-str" : "tok-com";
        out.push(`<span class="${cls}">${escapeHtml(part)}</span>`);
        continue;
      }
      let safe = escapeHtml(part);
      safe = safe.replace(kind === "c" ? C_KEYWORDS : RUST_KEYWORDS,
        (m) => `<span class="tok-key">${m}</span>`);
      safe = safe.replace(TYPES, (m) => `<span class="tok-type">${m}</span>`);
      safe = safe.replace(/\b(\d[\w.]*)\b/g, (m) => `<span class="tok-num">${m}</span>`);
      out.push(safe);
    }
    return out.join("");
  }

  function renderCode(text, kind) {
    if (!text) {
      el.output.innerHTML =
        '<div class="placeholder">No output — see the diagnostics below.</div>';
      return;
    }
    const lines = text.replace(/\n$/, "").split("\n");
    const body = lines
      .map((line, i) =>
        `<span class="ln">${i + 1}</span>${highlight(line, kind) || "&nbsp;"}`)
      .join("\n");
    el.output.innerHTML = `<pre style="margin:0"><code>${body}</code></pre>`;
  }

  function renderFileTabs() {
    el.filetabs.innerHTML = "";
    if (state.files.length < 2) return;
    for (const file of state.files) {
      const tab = document.createElement("button");
      tab.className = "filetab";
      tab.type = "button";
      tab.textContent = file.path;
      tab.setAttribute("aria-selected", String(file.path === state.activeFile));
      tab.addEventListener("click", () => {
        state.activeFile = file.path;
        renderCode(file.content, "rust");
        renderFileTabs();
      });
      el.filetabs.appendChild(tab);
    }
  }

  function renderDiagnostics(result) {
    el.diagnostics.innerHTML = "";
    if (!result.diagnostics) return;
    const kind = result.ok ? "warn" : "error";
    const box = document.createElement("pre");
    box.className = `diag diag--${kind}`;
    box.textContent = result.diagnostics;
    el.diagnostics.appendChild(box);
  }

  function setLane(text, variant) {
    if (!text) { el.lane.hidden = true; return; }
    el.lane.hidden = false;
    el.lane.textContent = text;
    el.lane.className = `chip chip--${variant}`;
  }

  function setStatus(text, spinning) {
    el.status.innerHTML = "";
    if (spinning) {
      const dot = document.createElement("span");
      dot.className = "spin";
      el.status.appendChild(dot);
    }
    el.status.appendChild(document.createTextNode(text));
  }

  /* ------------------------------------------------------ sign-in wall -- */

  function showWall(detail, opts = {}) {
    el.output.innerHTML = "";
    const wall = document.createElement("div");
    wall.className = "wall";

    const chip = document.createElement("span");
    chip.className = "chip chip--live";
    chip.textContent = opts.chip || "sign in to continue";

    const heading = document.createElement("h3");
    heading.textContent = opts.title || "This one needs a live compile";

    const body = document.createElement("p");
    body.textContent = detail;

    wall.append(chip, heading, body);

    if (opts.showButton !== false) {
      const slot = document.createElement("div");
      slot.id = "gsi-inline";
      wall.appendChild(slot);
      // Render a second Google button inside the wall, where the decision is
      // actually being made -- asking someone to go hunting in the header is
      // how you lose them.
      queueMicrotask(() => renderGoogleButton(slot));
    }

    const why = document.createElement("p");
    why.className = "wall__why";
    why.textContent = opts.why ||
      "Results we already have are free and unmetered. Signing in only meters " +
      "source we have never compiled before.";
    wall.appendChild(why);

    el.output.appendChild(wall);
  }

  /* ------------------------------------------------------------ auth -- */

  function renderGoogleButton(target) {
    if (!window.google || !window.__GOOGLE_CLIENT_ID || !target) return;
    try {
      google.accounts.id.renderButton(target, {
        theme: matchMedia("(prefers-color-scheme: dark)").matches
          ? "filled_black" : "outline",
        size: "medium",
        text: "signin_with",
        shape: "rectangular",
      });
    } catch (err) {
      console.warn("google button failed", err);
    }
  }

  function onCredential(response) {
    state.token = response.credential;
    sessionStorage.setItem("emitrust_token", state.token);
    refreshAccount().then(() => {
      setStatus("Signed in. Run it again.", false);
      if (state.pendingRun) { state.pendingRun = false; run(); }
    });
  }

  async function initAuth() {
    let health;
    try {
      health = await (await fetch("/api/health")).json();
    } catch {
      return;
    }
    if (!health.auth_enabled) {
      el.gsi.innerHTML =
        '<span class="meter">live compiles offline</span>';
      return;
    }
    // The client id is public by design; serve it from /api/health so the
    // page has no build step.
    window.__GOOGLE_CLIENT_ID = health.google_client_id || null;
    if (!window.__GOOGLE_CLIENT_ID || !window.google) return;
    google.accounts.id.initialize({
      client_id: window.__GOOGLE_CLIENT_ID,
      callback: onCredential,
      auto_select: false,
    });
    renderGoogleButton(el.gsi);
    await refreshAccount();
  }

  async function refreshAccount() {
    const headers = state.token
      ? { Authorization: `Bearer ${state.token}` } : {};
    let me;
    try {
      me = await (await fetch("/api/me", { headers })).json();
    } catch {
      return;
    }
    if (!me.signed_in) {
      el.meter.textContent = "";
      el.signout.hidden = true;
      el.gsi.hidden = false;
      return;
    }
    el.gsi.hidden = true;
    el.signout.hidden = false;
    const t = me.trial || {};
    el.meter.innerHTML =
      `<b>${t.remaining_today ?? 0}</b> of ${t.daily_limit ?? 0} live compiles left today`;
  }

  el.signout.addEventListener("click", () => {
    state.token = null;
    sessionStorage.removeItem("emitrust_token");
    if (window.google) google.accounts.id.disableAutoSelect();
    refreshAccount();
    setStatus("Signed out. Cached examples still work.", false);
  });

  /* --------------------------------------------------------- examples -- */

  async function loadExamples() {
    let data;
    try {
      data = await (await fetch("/api/examples")).json();
    } catch {
      el.examples.innerHTML =
        '<div class="placeholder">Could not reach the service.</div>';
      return;
    }
    state.examples = data.examples || [];
    el.examples.innerHTML = "";
    for (const ex of state.examples) {
      const button = document.createElement("button");
      button.className = "example";
      button.type = "button";
      button.setAttribute("aria-current", "false");

      const title = document.createElement("span");
      title.className = "example__title";
      title.append(document.createTextNode(ex.title));
      const dot = document.createElement("span");
      dot.className = ex.cached ? "dot" : "dot dot--live";
      dot.title = ex.cached ? "cached — free" : "not cached — needs a live compile";
      title.appendChild(dot);

      const blurb = document.createElement("span");
      blurb.className = "example__blurb";
      blurb.textContent = ex.blurb;

      button.append(title, blurb);
      button.addEventListener("click", () => selectExample(ex));
      el.examples.appendChild(button);
    }
    if (state.examples.length) selectExample(state.examples[0]);
  }

  function selectExample(ex) {
    state.activeSlug = ex.slug;
    el.source.value = ex.source;
    writeOptions(ex.options);
    for (const [i, node] of [...el.examples.children].entries()) {
      node.setAttribute?.("aria-current",
        String(state.examples[i]?.slug === ex.slug));
    }
    run();
  }

  /* ---------------------------------------------------------- compile -- */

  async function run() {
    if (state.busy) return;
    const source = el.source.value;
    if (!source.trim()) {
      setStatus("Nothing to compile.", false);
      return;
    }

    state.busy = true;
    el.run.disabled = true;
    setStatus("Transpiling…", true);
    setLane(null);
    el.diagnostics.innerHTML = "";

    const headers = { "Content-Type": "application/json" };
    if (state.token) headers.Authorization = `Bearer ${state.token}`;

    let response, payload;
    try {
      response = await fetch("/api/compile", {
        method: "POST",
        headers,
        body: JSON.stringify({ source, options: readOptions() }),
      });
      payload = await response.json();
    } catch {
      setStatus("The service is unreachable.", false);
      state.busy = false;
      el.run.disabled = false;
      return;
    }

    state.busy = false;
    el.run.disabled = false;

    if (response.status === 401) {
      state.pendingRun = true;
      setStatus("Sign in to compile this.", false);
      setLane("live compile", "live");
      showWall(payload.detail || "Sign in with Google to start your free trial.");
      return;
    }
    if (response.status === 429) {
      setStatus("Trial allowance used up.", false);
      setLane("limit reached", "warn");
      showWall(payload.detail || "You've used today's live compiles.", {
        chip: "trial limit",
        title: "That's today's allowance",
        showButton: false,
        why: "Cached results — including every example on the left — stay free " +
             "and unmetered. Your live allowance refills within 24 hours.",
      });
      refreshAccount();
      return;
    }
    if (response.status === 503) {
      setStatus("Live compiling is offline.", false);
      setLane("offline", "warn");
      showWall(payload.detail || "Live compiles are disabled on this deployment.", {
        chip: "unavailable",
        title: "Live compiling is off right now",
        showButton: false,
        why: "This is a server-side configuration issue, not something you did. " +
             "Cached examples still work normally.",
      });
      return;
    }
    if (!response.ok) {
      setStatus("Request rejected.", false);
      el.output.innerHTML = "";
      const box = document.createElement("pre");
      box.className = "diag diag--error";
      box.textContent = payload.detail || `HTTP ${response.status}`;
      el.output.appendChild(box);
      return;
    }

    // Success -- either lane.
    setLane(payload.cached ? "cached · free" : "live compile",
            payload.cached ? "free" : "live");

    state.files = payload.files || [];
    state.activeFile = state.files.length
      ? (state.files.find((f) => /main\.rs|lib\.rs/.test(f.path)) || state.files[0]).path
      : null;

    const emit = payload.emit || "rust";
    el.outname.textContent =
      emit === "crate" ? (state.activeFile || "crate")
      : emit === "mlir" ? "module.mlir"
      : emit === "actor-plan" ? "actor-plan"
      : "src/main.rs";

    const shown = state.activeFile
      ? (state.files.find((f) => f.path === state.activeFile)?.content ?? payload.output)
      : payload.output;

    renderCode(shown, emit === "mlir" || emit === "actor-plan" ? "mlir" : "rust");
    renderFileTabs();
    renderDiagnostics(payload);

    const ms = payload.duration_ms;
    setStatus(
      payload.cached
        ? "From cache — this one was free."
        : `Compiled in ${ms} ms.`,
      false,
    );
    if (payload.truncated) {
      setStatus("Output truncated — it was very large.", false);
    }
    if (!payload.cached) refreshAccount();
  }

  /* ------------------------------------------------------------ wire -- */

  el.run.addEventListener("click", run);
  $("language").addEventListener("change", syncSourceName);

  // Ctrl/Cmd+Enter is the expected gesture in a code box.
  el.source.addEventListener("keydown", (e) => {
    if ((e.metaKey || e.ctrlKey) && e.key === "Enter") { e.preventDefault(); run(); }
  });

  // Editing the source means it is no longer the pinned example.
  el.source.addEventListener("input", () => {
    el.srcmeta.textContent = "edited";
    setLane(null);
  });

  window.addEventListener("load", () => {
    initAuth();
    loadExamples();
  });
})();
