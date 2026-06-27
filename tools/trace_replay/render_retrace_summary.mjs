#!/usr/bin/env node

import { mkdir, readdir, readFile, stat, writeFile } from "node:fs/promises";
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
  for (const token of ["HA27Q3LQ", "JNHUVK4XL7ZLWK7L"]) {
    if (text.includes(token)) return token;
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
  for (const png of pngs) {
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

async function embedPng(source) {
  if (!source) return "";
  if (!(await isUsablePng(source))) return "";
  const data = await readFile(source);
  return `data:image/png;base64,${data.toString("base64")}`;
}

function statusText(row) {
  if (!row) return "NO RESULT";
  if (row.status === "NO_RESULT") return "NO RESULT";
  if (row.status === "FIXTURE_MISSING") return "MISSING FIXTURE";
  if (row.ssim) {
    const value = Number(row.ssim);
    const text = Number.isFinite(value) ? value.toFixed(4) : row.ssim;
    return `${row.status} <span>SSIM ${htmlEscape(text)}</span>`;
  }
  return row.status;
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

  const thumb = (src, label) => `
    <figure class="thumb">
      <figcaption>${htmlEscape(label)}</figcaption>
      ${src ? `<button class="image-button" type="button" aria-label="Open ${htmlEscape(label)} image"><img src="${htmlEscape(src)}" alt="${htmlEscape(label)}"></button>` : `<div class="no-image">NO IMAGE</div>`}
    </figure>`;

  const missingImages = (row) => !row.actualSrc || !row.goldenSrc || !row.diffSrc;
  const cellFor = (group, backend, caseName) => cells.get(`${group}\0${backend}\0${caseName}`) ?? {
    status: "NO_RESULT",
    actualSrc: "",
    goldenSrc: "",
    diffSrc: "",
  };

  const backendCell = (group, backend, caseName) => {
    const row = cellFor(group, backend, caseName);
    const statusClass = row.status === "PASS" ? "pass" : row.status === "NO_RESULT" || row.status === "FIXTURE_MISSING" ? "missing" : "fail";
    const imageClass = missingImages(row) ? "image-missing" : "";
    return `
      <section class="backend-cell ${statusClass} ${imageClass}">
        <header>${backend} <strong>${statusText(row)}</strong></header>
        <div class="thumbs">
          ${thumb(row.actualSrc, "actual")}
          ${thumb(row.goldenSrc, "golden")}
          ${thumb(row.diffSrc, "diff")}
        </div>
      </section>`;
  };

  const groupSections = groups.map((group) => {
    const displayGroup = groupLabels.get(group) ?? group;
    const groupCells = CASES.flatMap((caseName) => BACKENDS.map((backend) => cellFor(group, backend, caseName)));
    const passed = groupCells.filter((row) => row.status === "PASS").length;
    const failed = groupCells.filter((row) => row.status === "FAIL").length;
    const noResult = groupCells.filter((row) => row.status !== "PASS" && row.status !== "FAIL").length;
    const passRateDenominator = passed + failed;
    const passRate = passRateDenominator === 0 ? "0.0%" : `${(passed * 100 / passRateDenominator).toFixed(1)}%`;
    return `
      <section class="group">
        <h1>${htmlEscape(title)} - ${htmlEscape(displayGroup)}</h1>
        <p>Each backend cell shows actual / golden / diff. Failed cases are red; absent results are orange; incomplete image sets get an orange notch.<br>PASS ${passed}, FAIL ${failed}, NO RESULT ${noResult}, PASS RATE ${passRate}.</p>
        <div class="grid header-row">
          <div>case</div>
          <div>DirectGLES</div>
          <div>DirectVulkan</div>
        </div>
        ${CASES.map((caseName, index) => `
          <div class="grid case-row ${index % 2 === 0 ? "even" : "odd"}">
            <aside>
              <strong>${htmlEscape(shortCase(caseName))}</strong>
              ${caseName.includes("iterationt") && !rowMap.has(`${group}\0DirectGLES\0${caseName}`) && !rowMap.has(`${group}\0DirectVulkan\0${caseName}`)
                ? `<small>LFS fixture may be unavailable</small>` : ""}
            </aside>
            ${backendCell(group, "DirectGLES", caseName)}
            ${backendCell(group, "DirectVulkan", caseName)}
          </div>`).join("")}
      </section>`;
  }).join("");

  const html = `<!doctype html>
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
  main { width: 1170px; margin: 0 auto; padding: 14px 10px 24px; }
  .group { break-after: page; margin-bottom: 28px; }
  .group:last-child { break-after: auto; }
  h1 { margin: 0; font-size: 24px; line-height: 1.2; }
  p { margin: 4px 0 12px; color: var(--muted); }
  .grid {
    display: grid;
    grid-template-columns: 280px 430px 430px;
    column-gap: 10px;
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
    display: grid;
    place-items: center;
    padding: 28px;
    background: rgb(0 0 0 / 86%);
    cursor: zoom-out;
  }
  .lightbox[hidden] {
    display: none;
  }
  .lightbox img {
    max-width: min(96vw, 1600px);
    max-height: 94vh;
    object-fit: contain;
    box-shadow: 0 18px 70px rgb(0 0 0 / 70%);
  }
  @page { size: 1170px 2380px; margin: 0; }
  @media print {
    main { padding: 10px; }
    .group { margin-bottom: 0; }
    .lightbox { display: none; }
  }
</style>
</head>
<body>
<main>
${groupSections}
</main>
<div class="lightbox" id="lightbox" hidden>
  <img id="lightbox-image" alt="">
</div>
<script>
(() => {
  const lightbox = document.getElementById("lightbox");
  const lightboxImage = document.getElementById("lightbox-image");
  const closeLightbox = () => {
    lightbox.hidden = true;
    lightboxImage.removeAttribute("src");
    lightboxImage.alt = "";
  };

  document.querySelectorAll(".image-button").forEach((button) => {
    button.addEventListener("click", () => {
      const image = button.querySelector("img");
      if (!image) return;
      lightboxImage.src = image.src;
      lightboxImage.alt = image.alt;
      lightbox.hidden = false;
    });
  });

  lightbox.addEventListener("click", closeLightbox);
  document.addEventListener("keydown", (event) => {
    if (event.key === "Escape" && !lightbox.hidden) closeLightbox();
  });
})();
</script>
</body>
</html>
`;
  const output = path.join(outputDir, htmlName);
  await writeFile(output, html, "utf8");
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
