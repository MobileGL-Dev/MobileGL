#!/usr/bin/env node

import { createWriteStream } from "node:fs";
import { mkdir, readdir, readFile, stat } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const BACKENDS = ["DirectGLES", "DirectVulkan"];
const DEFAULT_FIXTURE_ROOT = path.join(path.dirname(fileURLToPath(import.meta.url)), "fixtures");
const CASES = [
  "OpenRA",
  "minecraft-1.21.4-startup",
  "minecraft-1.21.4-main-menu",
  "minecraft-1.21.4-in-world",
  "minecraft-1.21.4-fabric-sodium-in-world",
  "minecraft-1.21.4-fabric-rei-inventory-normal-world",
  "minecraft-1.21.4-fabric-xaero-minimap-in-world-normal-world",
  "minecraft-1.21.4-fabric-xaero-world-map-in-world-normal-world",
  "minecraft-1.21.4-fabric-journeymap-in-world-normal-world",
  "minecraft-1.21.4-fabric-modernui-inventory-normal-world",
  "minecraft-1.21.4-fabric-iris-bsl-in-world",
  "minecraft-1.21.4-fabric-iris-makeup-ultrafast-in-world",
  "minecraft-1.21.4-fabric-iris-super-duper-vanilla-in-world",
  "minecraft-1.21.4-fabric-iris-sundial-lite-in-world",
  "minecraft-1.21.4-fabric-iris-complementary-reimagined-in-world",
  "minecraft-1.21.4-fabric-iris-complementary-unbound-in-world",
  "minecraft-1.21.4-fabric-iris-mellow-in-world",
  "minecraft-1.21.4-fabric-iris-nostalgia-in-world",
  "minecraft-1.21.4-fabric-iris-bliss-in-world",
  "minecraft-1.21.4-fabric-iris-chocapic-v6-lite-in-world",
  "minecraft-1.21.4-fabric-iris-iterationt-in-world",
  "minecraft-1.21.4-fabric-iris-iterationt-nodsa-in-world",
  "minecraft-1.21.4-fabric-iris-photon-v1.1-in-world",
  "minecraft-1.21.4-fabric-iris-photon-v1.3b-in-world",
  "minecraft-1.21.4-fabric-iris-derivative-main-d24.4.14-in-world",
];

function usage() {
  console.error(`Usage:
  node tools/trace_replay/render_retrace_summary.mjs \\
    --input DIR [--input DIR ...] \\
    --output-dir DIR \\
    [--title TITLE] [--group-label LABEL] [--html NAME]`);
}

function parseArgs(argv) {
  const args = {
    inputs: [],
    outputDir: "",
    title: "MobileGL retrace overview",
    groupLabel: "retrace",
    htmlName: "mobilegl-retrace-overview.html",
  };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    const value = argv[i + 1];
    if (arg === "--input" && value) {
      args.inputs.push(path.resolve(value));
      i += 1;
    } else if (arg === "--output-dir" && value) {
      args.outputDir = path.resolve(value);
      i += 1;
    } else if (arg === "--title" && value) {
      args.title = value;
      i += 1;
    } else if (arg === "--group-label" && value) {
      args.groupLabel = value;
      i += 1;
    } else if (arg === "--html" && value) {
      args.htmlName = value;
      i += 1;
    } else if (arg === "--help" || arg === "-h") {
      usage();
      process.exit(0);
    } else {
      throw new Error(`Unknown or incomplete argument: ${arg}`);
    }
  }
  if (args.inputs.length === 0 || !args.outputDir) {
    usage();
    process.exit(2);
  }
  return args;
}

async function exists(filePath) {
  try {
    await stat(filePath);
    return true;
  } catch {
    return false;
  }
}

async function isUsablePng(filePath) {
  try {
    const info = await stat(filePath);
    return info.isFile() && filePath.toLowerCase().endsWith(".png") && info.size > 1024;
  } catch {
    return false;
  }
}

async function walk(root) {
  const results = [];
  if (!(await exists(root))) return results;
  const entries = await readdir(root, { withFileTypes: true });
  for (const entry of entries) {
    const fullPath = path.join(root, entry.name);
    if (entry.isDirectory()) {
      results.push(...await walk(fullPath));
    } else if (entry.isFile()) {
      results.push(fullPath);
    }
  }
  return results;
}

function safeCase(caseName) {
  return caseName.replace(/[^A-Za-z0-9._-]/g, "_");
}

function shortCase(caseName) {
  return caseName
    .replace(/^minecraft-1\.21\.4-/, "mc-")
    .replaceAll("fabric-iris-", "")
    .replaceAll("fabric-", "")
    .replace(/-in-world$/, "");
}

function normalizeSlashes(value) {
  return value.split(path.sep).join("/");
}

function inferCaseBackend(filePath) {
  const text = normalizeSlashes(filePath);
  const backend = BACKENDS.find((candidate) => text.includes(candidate)) ?? "";
  const sortedCases = [...CASES].sort((a, b) => b.length - a.length);
  const caseName = sortedCases.find((candidate) =>
    text.includes(candidate) || text.includes(safeCase(candidate))
  ) ?? "";
  return { caseName, backend };
}

function inferGroup(filePath, defaultGroup) {
  const text = normalizeSlashes(filePath);
  for (const segment of text.split("/")) {
    const match = segment.match(/^android-retrace-(?:result|fixture|work)-(.+)$/);
    if (!match) continue;
    const parts = match[1].split("-");
    const serial = parts[parts.length - 1];
    if (serial) return serial;
  }
  return defaultGroup;
}

function parseGpuLabel(logText) {
  const renderer = logText.match(/^\[[^\n]*\]\s*\[[^\n]*\]:\s*GL_RENDERER:\s*(.+?)\s*$/m)
    ?? logText.match(/^GL_RENDERER:\s*(.+?)\s*$/m);
  if (renderer?.[1]) return renderer[1].trim();

  const vulkanDevice = logText.match(/^\[[^\n]*\]\s*\[[^\n]*\]:\s+(.+?)\s+\(Vulkan\s+[^,\n]+,\s*[^)\n]*GPU\)\s*$/m)
    ?? logText.match(/^\s+(.+?)\s+\(Vulkan\s+[^,\n]+,\s*[^)\n]*GPU\)\s*$/m);
  if (vulkanDevice?.[1]) return vulkanDevice[1].trim();

  return "";
}

async function collectGroupLabels(files, defaultGroup) {
  const labels = new Map();
  const logFiles = files.filter((file) => {
    const name = path.basename(file).toLowerCase();
    return name === "mobilegl.log" || name === "logcat.txt";
  });
  for (const logFile of logFiles) {
    const group = inferGroup(logFile, defaultGroup);
    try {
      const gpuLabel = parseGpuLabel(await readFile(logFile, "utf8"));
      if (gpuLabel) labels.set(group, gpuLabel);
    } catch {
      // Diagnostics should not make summary rendering fail.
    }
  }

  return labels;
}

function scoreImage(filePath, caseName, backend, kind) {
  const text = normalizeSlashes(filePath);
  const name = path.basename(filePath).toLowerCase();
  const fixtureGoldenPrefix = `${caseName}.`.toLowerCase();
  const lowerKind = kind.toLowerCase();
  if (lowerKind === "golden" && name.includes("alternate-golden")) return 0;
  const matchesKind =
    name === `${lowerKind}.png` ||
    name.endsWith(`-${lowerKind}.png`) ||
    name.endsWith(`_${lowerKind}.png`);
  const matchesFixtureGolden =
    (lowerKind === "golden" || lowerKind === "alternate-golden") &&
    name.startsWith(fixtureGoldenPrefix) &&
    name.endsWith(".png");
  if (!matchesKind && !matchesFixtureGolden) return 0;

  let score = 0;
  if (text.includes(caseName) || text.includes(safeCase(caseName))) score += 4;
  if (text.includes(backend)) score += 3;
  score += 3;
  if (matchesFixtureGolden) {
    score += 4;
    if (lowerKind === "alternate-golden" && name.includes("-mali.")) score += 3;
    if (lowerKind === "golden" && !name.includes("-mali.")) score += 2;
  }
  if (name === `${caseName}-${backend}-${kind}.png`.toLowerCase()) score += 4;
  if (name === `${safeCase(caseName)}-${backend}-${kind}.png`.toLowerCase()) score += 4;
  if (name === `${kind}.png`) score += 4;
  return score;
}

async function findImage(pngs, caseName, backend, kind, nearFile) {
  const exactNames = [
    `${caseName}-${backend}-${kind}.png`,
    `${safeCase(caseName)}-${backend}-${kind}.png`,
    `${caseName}-${kind}.png`,
    `${safeCase(caseName)}-${kind}.png`,
    `${kind}.png`,
  ];

  if (nearFile) {
    let current = path.dirname(nearFile);
    for (let depth = 0; depth < 5; depth += 1) {
      for (const name of exactNames) {
        const candidate = path.join(current, name);
        if (await isUsablePng(candidate)) return candidate;
      }
      const parent = path.dirname(current);
      if (parent === current) break;
      current = parent;
    }
  }

  const scored = [];
  const nearGroup = nearFile ? inferGroup(nearFile, "") : "";
  for (const png of pngs) {
    const text = normalizeSlashes(png);
    if ((kind === "actual" || kind === "diff") && backend && !text.includes(backend)) continue;
    const pngGroup = inferGroup(png, "");
    if (nearGroup && pngGroup && pngGroup !== nearGroup) continue;
    const score = scoreImage(png, caseName, backend, kind);
    if (score >= 7) scored.push({ score, png });
  }
  scored.sort((a, b) => b.score - a.score || a.png.length - b.png.length);
  return scored[0]?.png ?? "";
}

async function collectRows(inputs, defaultGroup) {
  const files = [];
  for (const input of inputs) files.push(...await walk(input));
  const groupLabels = await collectGroupLabels(files, defaultGroup);
  const fixtureFiles = await walk(DEFAULT_FIXTURE_ROOT);
  const pngs = [];
  for (const file of [...files, ...fixtureFiles]) {
    if (await isUsablePng(file)) pngs.push(file);
  }

  const rows = new Map();

  for (const resultJson of files.filter((file) => path.basename(file) === "result.json" || file.endsWith("-result.json"))) {
    let { caseName, backend } = inferCaseBackend(resultJson);
    let result = {};
    try {
      result = JSON.parse(await readFile(resultJson, "utf8"));
      backend = backend || result.backend || "";
    } catch (error) {
      result = { passed: false, message: `failed to parse result.json: ${error.message}` };
    }
    if (!caseName || !BACKENDS.includes(backend)) continue;
    const group = inferGroup(resultJson, defaultGroup);
    const key = `${group}\0${backend}\0${caseName}`;
    const matchedGolden = result.matchedGoldenPath || "";
    const alternateGoldens = Array.isArray(result.alternateGoldenPaths) ? result.alternateGoldenPaths : [];
    const useAlternateGolden = alternateGoldens.some((alternate) =>
      path.basename(String(alternate)) === path.basename(String(matchedGolden))
    ) || String(matchedGolden).includes("alternate-golden");
    rows.set(key, {
      group,
      backend,
      caseName,
      status: result.passed ? "PASS" : "FAIL",
      ssim: result.ssim === undefined || result.ssim === null ? "" : String(result.ssim),
      message: result.message || "",
      actual: await findImage(pngs, caseName, backend, "actual", resultJson),
      golden: await findImage(pngs, caseName, backend, useAlternateGolden ? "alternate-golden" : "golden", resultJson),
      diff: await findImage(pngs, caseName, backend, "diff", resultJson),
    });
  }

  // Preserve visibility for jobs that uploaded logs/diagnostics but never wrote result.json.
  const directories = new Set(files.map((file) => path.dirname(file)));
  for (const directory of directories) {
    const { caseName, backend } = inferCaseBackend(directory);
    if (!caseName || !BACKENDS.includes(backend)) continue;
    const group = inferGroup(directory, defaultGroup);
    const key = `${group}\0${backend}\0${caseName}`;
    if (rows.has(key)) continue;
    rows.set(key, {
      group,
      backend,
      caseName,
      status: "NO_RESULT",
      ssim: "",
      message: "result.json was not found",
      actual: await findImage(pngs, caseName, backend, "actual", directory),
      golden: await findImage(pngs, caseName, backend, "golden", directory),
      diff: await findImage(pngs, caseName, backend, "diff", directory),
    });
  }

  return { rows: [...rows.values()], groupLabels };
}

function htmlEscape(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;");
}

function jsonScriptText(value) {
  return JSON.stringify(value).replaceAll("<", "\\u003c");
}

async function embedPng(source) {
  if (!source) return "";
  if (!(await isUsablePng(source))) return "";
  const data = await readFile(source);
  return `data:image/png;base64,${data.toString("base64")}`;
}

function statusText(row) {
  if (!row) return "NO RESULT";
  const effective = effectiveStatus(row);
  if (effective === "FAIL" && row.status === "NO_RESULT") return `FAIL <span>NO RESULT</span>`;
  if (row.status === "NO_RESULT") return "NO RESULT";
  if (row.status === "FIXTURE_MISSING") return "MISSING FIXTURE";
  if (row.ssim) {
    const value = Number(row.ssim);
    const text = Number.isFinite(value) ? value.toFixed(4) : row.ssim;
    return `${row.status} <span>SSIM ${htmlEscape(text)}</span>`;
  }
  return row.status;
}

function effectiveStatus(row) {
  if (!row) return "NO_RESULT";
  if (row.status === "PASS" || row.status === "FAIL") return row.status;
  return !row.actualSrc && !row.goldenSrc ? "NO_RESULT" : "FAIL";
}

async function renderHtml(rows, groupLabels, outputDir, title, groupLabel, htmlName) {
  await mkdir(outputDir, { recursive: true });
  const groups = [...new Set(rows.map((row) => row.group))].sort();
  if (groups.length === 0) groups.push(groupLabel);
  const rowMap = new Map(rows.map((row) => [`${row.group}\0${row.backend}\0${row.caseName}`, row]));

  const cells = new Map();
  for (const row of rows) {
    cells.set(`${row.group}\0${row.backend}\0${row.caseName}`, {
      ...row,
      actualSrc: await embedPng(row.actual),
      goldenSrc: await embedPng(row.golden),
      diffSrc: await embedPng(row.diff),
    });
  }

  const imageTemplates = [];
  const imageTemplatesByGroup = new Map();
  const imageTemplateForGroup = (group) => {
    let template = imageTemplatesByGroup.get(group);
    if (!template) {
      template = { id: `image-data-${imageTemplates.length}`, images: {}, nextImage: 0 };
      imageTemplatesByGroup.set(group, template);
      imageTemplates.push(template);
    }
    return template;
  };
  const registerImage = (group, src) => {
    if (!src) return "";
    const template = imageTemplateForGroup(group);
    const id = `image-${template.nextImage}`;
    template.nextImage += 1;
    template.images[id] = src;
    return id;
  };

  const thumb = (group, src, label) => {
    const imageId = registerImage(group, src);
    return `
    <figure class="thumb">
      <figcaption>${htmlEscape(label)}</figcaption>
      ${imageId ? `<button class="image-button" type="button" aria-label="Open ${htmlEscape(label)} image"><img class="lazy-image" data-image-id="${imageId}" alt="${htmlEscape(label)}" loading="lazy" decoding="async"></button>` : `<div class="no-image">NO IMAGE</div>`}
    </figure>`;
  };

  const missingImages = (row) => !row.actualSrc || !row.goldenSrc || !row.diffSrc;
  const cellFor = (group, backend, caseName) => cells.get(`${group}\0${backend}\0${caseName}`) ?? {
    status: "NO_RESULT",
    actualSrc: "",
    goldenSrc: "",
    diffSrc: "",
  };

  const backendCell = (group, backend, caseName) => {
    const row = cellFor(group, backend, caseName);
    const effective = effectiveStatus(row);
    const statusClass = effective === "PASS" ? "pass" : effective === "NO_RESULT" ? "missing" : "fail";
    const imageClass = missingImages(row) ? "image-missing" : "";
    return `
      <section class="backend-cell ${statusClass} ${imageClass}">
        <header>${backend} <strong>${statusText(row)}</strong></header>
        <div class="thumbs">
          ${thumb(group, row.actualSrc, "actual")}
          ${thumb(group, row.goldenSrc, "golden")}
          ${thumb(group, row.diffSrc, "diff")}
        </div>
      </section>`;
  };

  const output = path.join(outputDir, htmlName);
  const stream = createWriteStream(output, { encoding: "utf8" });
  const write = (chunk) => new Promise((resolve, reject) => {
    stream.write(chunk, (error) => error ? reject(error) : resolve());
  });
  const finish = () => new Promise((resolve, reject) => {
    stream.end((error) => error ? reject(error) : resolve());
  });

  await write(`<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>${htmlEscape(title)}</title>
<style>
  :root {
    color-scheme: dark;
    --bg: #141414;
    --row-a: #1e1e1e;
    --row-b: #242424;
    --panel: #181818;
    --image-bg: #242424;
    --line: #3a3a3a;
    --text: #e8e8e8;
    --muted: #aaa;
    --green: #1a9453;
    --red: #d22d2d;
    --orange: #cd841c;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0;
    background: var(--bg);
    color: var(--text);
    font-family: Arial, Helvetica, sans-serif;
    font-size: 12px;
  }
  main { width: min(1170px, 100%); margin: 0 auto; padding: 14px 10px 96px; }
  .document-title {
    margin: 0 0 6px;
    font-size: 28px;
    line-height: 1.2;
  }
  .overview-note {
    margin: 0 0 16px;
    color: var(--muted);
  }
  .group { margin-bottom: 28px; }
  .group-title {
    display: flex;
    align-items: start;
    justify-content: space-between;
    gap: 16px;
    margin-bottom: 10px;
    padding: 12px 14px;
    border: 1px solid var(--line);
    border-radius: 8px;
    background: #2f2f2f;
    cursor: pointer;
    user-select: none;
  }
  .group-title:hover { border-color: #5a6473; }
  .group-title:focus-visible {
    outline: 2px solid #6f87ff;
    outline-offset: 2px;
  }
  .group-title::after {
    content: "+ Expand";
    flex: 0 0 auto;
    align-self: center;
    color: var(--muted);
    font-weight: 700;
  }
  .group:not(.collapsed) .group-title::after {
    content: "- Collapse";
  }
  h1 { margin: 0; font-size: 24px; line-height: 1.2; }
  p { margin: 4px 0 12px; color: var(--muted); }
  .stats {
    display: flex;
    flex-wrap: wrap;
    gap: 8px;
    margin: 8px 0 12px;
  }
  .stat {
    border: 1px solid var(--line);
    border-radius: 6px;
    padding: 5px 8px;
    color: var(--text);
    background: #151922;
    font-weight: 700;
  }
  .stat strong {
    margin-left: 5px;
    font-size: 14px;
  }
  .stat.rate {
    border-color: #6f87ff;
    background: #1d2544;
  }
  .stat-break {
    flex-basis: 100%;
    height: 0;
  }
  .stat.pass {
    border-color: var(--green);
    background: #15341e;
  }
  .stat.fail {
    border-color: var(--red);
    background: #3a171a;
  }
  .stat.no-result {
    border-color: var(--orange);
    background: #3b2a10;
  }
  .status-bar {
    display: flex;
    width: min(560px, 100%);
    height: 14px;
    margin: -4px 0 12px;
    overflow: hidden;
    border: 1px solid var(--line);
    border-radius: 999px;
    background: #151922;
  }
  .status-bar .segment {
    min-width: 0;
    height: 100%;
  }
  .status-bar .pass { background: var(--green); }
  .status-bar .no-result { background: var(--orange); }
  .status-bar .fail { background: var(--red); }
  .status-bar .empty {
    width: 100%;
    background: #252b38;
  }
  .group.collapsed .case-list { display: none; }
  .case-list { overflow-x: auto; }
  .grid {
    display: grid;
    grid-template-columns: 280px 430px 430px;
    column-gap: 10px;
    min-width: 1150px;
  }
  .header-row {
    color: var(--muted);
    font-weight: 700;
    margin-bottom: 4px;
  }
  .case-row {
    min-height: 112px;
    padding: 5px 0;
    align-items: stretch;
  }
  .case-row.even { background: var(--row-a); }
  .case-row.odd { background: var(--row-b); }
  aside {
    padding: 22px 8px 0 2px;
    overflow-wrap: anywhere;
  }
  aside small {
    display: block;
    margin-top: 5px;
    color: var(--orange);
  }
  .backend-cell {
    border: 1px solid var(--line);
    background: var(--panel);
    min-height: 102px;
    position: relative;
  }
  .backend-cell.fail { border: 3px solid var(--red); }
  .backend-cell.missing { border: 2px solid var(--orange); }
  .backend-cell.image-missing::after {
    content: "";
    position: absolute;
    right: 0;
    top: 0;
    width: 0;
    height: 0;
    border-top: 16px solid var(--orange);
    border-left: 16px solid transparent;
  }
  .backend-cell > header {
    height: 22px;
    padding: 4px 7px;
    font-weight: 700;
    background: var(--green);
  }
  .backend-cell.fail > header { background: var(--red); }
  .backend-cell.missing > header { background: var(--orange); }
  .backend-cell header span { margin-left: 4px; }
  .thumbs {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 5px;
    padding: 5px;
  }
  .thumb {
    height: 74px;
    margin: 0;
    border: 1px solid var(--line);
    background: var(--image-bg);
    position: relative;
    overflow: hidden;
  }
  .thumb figcaption {
    position: absolute;
    left: 4px;
    top: 2px;
    z-index: 1;
    font-size: 9px;
    font-weight: 700;
  }
  .image-button {
    width: 100%;
    height: 100%;
    display: block;
    margin: 0;
    padding: 0;
    border: 0;
    background: transparent;
    cursor: zoom-in;
  }
  .thumb img {
    width: 100%;
    height: 100%;
    object-fit: contain;
    padding-top: 12px;
    display: block;
  }
  .no-image {
    height: 100%;
    display: grid;
    place-items: center;
    padding-top: 12px;
    font-weight: 700;
    color: var(--text);
  }
  .lightbox {
    position: fixed;
    inset: 0;
    z-index: 20;
    display: flex;
    flex-direction: column;
    padding: max(12px, env(safe-area-inset-top)) max(10px, env(safe-area-inset-right)) max(12px, env(safe-area-inset-bottom)) max(10px, env(safe-area-inset-left));
    background: rgb(0 0 0 / 86%);
    cursor: zoom-out;
  }
  .lightbox[hidden] {
    display: none;
  }
  .lightbox-close {
    position: fixed;
    right: max(10px, env(safe-area-inset-right));
    top: max(10px, env(safe-area-inset-top));
    z-index: 21;
    width: 38px;
    height: 38px;
    border: 1px solid rgb(255 255 255 / 24%);
    border-radius: 999px;
    color: var(--text);
    background: rgb(18 18 18 / 92%);
    font-size: 24px;
    line-height: 34px;
    cursor: pointer;
  }
  .lightbox-stage {
    width: 100%;
    height: 100%;
    display: grid;
    place-items: center;
    overflow: auto;
    overscroll-behavior: contain;
    -webkit-overflow-scrolling: touch;
    padding: 44px 0 16px;
  }
  .lightbox img {
    max-width: min(96vw, 1600px);
    max-height: calc(100dvh - 72px);
    object-fit: contain;
    box-shadow: 0 18px 70px rgb(0 0 0 / 70%);
    cursor: zoom-in;
  }
  .lightbox.zoomed .lightbox-stage {
    place-items: start center;
  }
  .lightbox.zoomed img {
    max-width: none;
    max-height: none;
    cursor: zoom-out;
  }
  .summary-footer {
    margin-top: 24px;
    padding: 18px 0 8px;
    color: var(--muted);
    border-top: 1px solid var(--line);
  }
  @page { size: 1170px 2380px; margin: 0; }
  @media print {
    main { padding: 10px; }
    .group { break-after: page; margin-bottom: 0; }
    .group:last-child { break-after: auto; }
    .summary-footer { display: none; }
    .lightbox { display: none; }
  }
  @media (max-width: 700px) {
    body { font-size: 11px; }
    main { padding: 10px 6px 72px; }
    .document-title { font-size: 22px; }
    .group-title {
      display: block;
      padding: 10px 10px 12px;
    }
    .group-title::after {
      display: block;
      margin-top: 8px;
    }
    h1 { font-size: 18px; }
    .stats { gap: 6px; }
    .stat { padding: 4px 6px; }
    .status-bar { margin-bottom: 0; }
    .lightbox {
      background: rgb(0 0 0 / 92%);
    }
    .lightbox-stage {
      padding-top: 52px;
      align-items: start;
    }
    .lightbox img {
      max-width: calc(100vw - 20px);
      max-height: calc(100dvh - 84px);
    }
  }
</style>
</head>
<body>
<main>
<h1 class="document-title">${htmlEscape(title)}</h1>
<p class="overview-note">Each backend cell shows actual / golden / diff. Failed cases are red; absent results are orange; incomplete image sets get an orange notch.</p>
`);

  const caseTemplates = [];
  for (const group of groups) {
    const displayGroup = groupLabels.get(group) ?? group;
    const templateId = `case-template-${caseTemplates.length}`;
    const groupCells = CASES.flatMap((caseName) => BACKENDS.map((backend) => cellFor(group, backend, caseName)));
    const passed = groupCells.filter((row) => effectiveStatus(row) === "PASS").length;
    const failed = groupCells.filter((row) => effectiveStatus(row) === "FAIL").length;
    const noResult = groupCells.filter((row) => effectiveStatus(row) === "NO_RESULT").length;
    const passRateDenominator = passed + failed;
    const passRate = passRateDenominator === 0 ? "0.0%" : `${(passed * 100 / passRateDenominator).toFixed(1)}%`;
    const statusTotal = passed + noResult + failed;
    const passPercent = statusTotal === 0 ? "0" : (passed * 100 / statusTotal).toFixed(3);
    const noResultPercent = statusTotal === 0 ? "0" : (noResult * 100 / statusTotal).toFixed(3);
    const failPercent = statusTotal === 0 ? "0" : (failed * 100 / statusTotal).toFixed(3);
    let caseHtml = `
          <div class="grid header-row">
            <div>case</div>
            <div>DirectGLES</div>
            <div>DirectVulkan</div>
          </div>
`;
    for (const [index, caseName] of CASES.entries()) {
      caseHtml += `
          <div class="grid case-row ${index % 2 === 0 ? "even" : "odd"}">
            <aside>
              <strong>${htmlEscape(shortCase(caseName))}</strong>
              ${caseName.includes("iterationt") && !rowMap.has(`${group}\0DirectGLES\0${caseName}`) && !rowMap.has(`${group}\0DirectVulkan\0${caseName}`)
                ? `<small>LFS fixture may be unavailable</small>` : ""}
            </aside>
            ${backendCell(group, "DirectGLES", caseName)}
            ${backendCell(group, "DirectVulkan", caseName)}
          </div>`;
    }
    caseTemplates.push({ id: templateId, html: caseHtml });
    const imageTemplate = imageTemplatesByGroup.get(group);
    await write(`
      <section class="group collapsed" data-template-id="${templateId}" data-image-template-id="${imageTemplate ? imageTemplate.id : ""}">
        <div class="group-title" role="button" tabindex="0" aria-expanded="false">
          <div>
            <h1>${htmlEscape(displayGroup)}</h1>
            <div class="stats" aria-label="summary stats">
              <span class="stat rate">PASS RATE <strong>${passRate}</strong></span>
              <span class="stat-break" aria-hidden="true"></span>
              <span class="stat pass">PASS <strong>${passed}</strong></span>
              <span class="stat no-result">NO RESULT <strong>${noResult}</strong></span>
              <span class="stat fail">FAIL <strong>${failed}</strong></span>
            </div>
            <div class="status-bar" aria-label="PASS ${passed}, NO RESULT ${noResult}, FAIL ${failed}">
              ${statusTotal === 0
                ? `<span class="empty" title="NO RESULT 0"></span>`
                : `<span class="segment pass" style="width: ${passPercent}%;" title="PASS ${passed}"></span><span class="segment no-result" style="width: ${noResultPercent}%;" title="NO RESULT ${noResult}"></span><span class="segment fail" style="width: ${failPercent}%;" title="FAIL ${failed}"></span>`}
            </div>
          </div>
        </div>
        <div class="case-list" data-loaded="false"></div>
      </section>`);
  }

  for (const template of caseTemplates) {
    await write(`
<script type="application/json" id="${template.id}">${jsonScriptText(template.html)}</script>`);
  }

  await write(`
<footer class="summary-footer">End of retrace summary. ${groups.length} ${htmlEscape(groupLabel)} groups rendered.</footer>
</main>
<div class="lightbox" id="lightbox" hidden>
  <button class="lightbox-close" type="button" aria-label="Close image">&times;</button>
  <div class="lightbox-stage">
    <img id="lightbox-image" alt="">
  </div>
</div>
<script>
(() => {
  const lightbox = document.getElementById("lightbox");
  const lightboxImage = document.getElementById("lightbox-image");
  const lightboxClose = document.querySelector(".lightbox-close");
  const imageDataCache = new Map();
  const hydrateGroup = (group) => {
    const list = group.querySelector(".case-list");
    if (!list || list.dataset.loaded === "true") return;
    const template = document.getElementById(group.dataset.templateId);
    if (!template) return;
    list.innerHTML = JSON.parse(template.textContent);
    list.dataset.loaded = "true";
    template.remove();
  };
  const imageDataForGroup = (group) => {
    const templateId = group?.dataset.imageTemplateId;
    if (!templateId) return {};
    if (imageDataCache.has(templateId)) return imageDataCache.get(templateId);
    const template = document.getElementById(templateId);
    if (!template) return {};
    const images = JSON.parse(template.textContent);
    imageDataCache.set(templateId, images);
    template.remove();
    return images;
  };
  const loadImage = (image) => {
    if (!image || image.src || !image.dataset.imageId) return;
    const group = image.closest(".group");
    const src = imageDataForGroup(group)[image.dataset.imageId];
    if (src) image.src = src;
  };
  const loadImagesInGroup = (group) => {
    hydrateGroup(group);
    group.querySelectorAll("img[data-image-id]").forEach(loadImage);
  };
  const closeLightbox = () => {
    lightbox.hidden = true;
    lightbox.classList.remove("zoomed");
    lightboxImage.removeAttribute("src");
    lightboxImage.alt = "";
  };

  document.addEventListener("click", (event) => {
    const button = event.target.closest(".image-button");
    if (!button) return;
    const image = button.querySelector("img");
    if (!image) return;
    loadImage(image);
    lightboxImage.src = image.src;
    lightboxImage.alt = image.alt;
    lightbox.hidden = false;
    lightbox.classList.remove("zoomed");
  });

  document.querySelectorAll(".group-title").forEach((title) => {
    const toggleGroup = () => {
      const group = title.closest(".group");
      const expanded = group.classList.toggle("collapsed") === false;
      title.setAttribute("aria-expanded", String(expanded));
      if (expanded) loadImagesInGroup(group);
    };
    title.addEventListener("click", toggleGroup);
    title.addEventListener("keydown", (event) => {
      if (event.key !== "Enter" && event.key !== " ") return;
      event.preventDefault();
      toggleGroup();
    });
  });

  lightbox.addEventListener("click", closeLightbox);
  lightboxClose.addEventListener("click", closeLightbox);
  lightboxImage.addEventListener("click", (event) => {
    event.stopPropagation();
    lightbox.classList.toggle("zoomed");
  });
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !lightbox.hidden) closeLightbox();
  });
  window.addEventListener("load", () => {
    document.querySelectorAll(".group:not(.collapsed)").forEach(loadImagesInGroup);
  }, { once: true });
})();
</script>
`);

  for (const template of imageTemplates) {
    await write(`
<script type="application/json" id="${template.id}" data-image-data="true">${jsonScriptText(template.images)}</script>`);
  }

  await write(`
</body>
</html>
`);
  await finish();
  return output;
}

async function main() {
  const args = parseArgs(process.argv.slice(2));
  await mkdir(args.outputDir, { recursive: true });
  const { rows, groupLabels } = await collectRows(args.inputs, args.groupLabel);
  const htmlPath = await renderHtml(rows, groupLabels, args.outputDir, args.title, args.groupLabel, args.htmlName);
  console.log(htmlPath);
}

main().catch((error) => {
  console.error(error.stack || error.message);
  process.exit(1);
});
