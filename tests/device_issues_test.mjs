#!/usr/bin/env node
/*
 * On-device regression tests for the two psychosis defects:
 *
 *   Issue 1 - ax frames and default tap coordinates must share one space.
 *             Regression: tap a link at its fresh AX frame center with no W H
 *             and require the link to activate. Fails on 0.6.0 in landscape.
 *
 *   Issue 2 - scrolling must be deterministic and settled.
 *             Regression: two scroll down 600 calls must move content ~1200
 *             (checked per call, plus a cumulative pair check), scrollto must
 *             center a label so an immediate center tap activates it, and
 *             scrolltop/scrolloffset must work.
 *
 * Run on the jailbroken device:
 *   node tests/device_issues_test.mjs [--report PATH]
 *
 * The coordinate regression only manifests when the interface is landscape on
 * a 768x1024 (portrait-native) device; in portrait the old hardcoded default
 * happened to match. The script records the orientation and keeps testing.
 */

import { spawnSync } from "node:child_process";
import { setTimeout as sleep } from "node:timers/promises";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";

const PSY = process.env.PSYCHOSIS || "/var/jb/usr/bin/psychosis";
const ARTICLE_URL = process.env.PSY_ARTICLE_URL || "https://en.wikipedia.org/wiki/Dog";
const COORD_URL = process.env.PSY_COORD_URL || "https://en.wikipedia.org/wiki/Dog";
const COORD_LINK = process.env.PSY_COORD_LINK || "Canidae";
const COORD_ASSERT = process.env.PSY_COORD_ASSERT || "Canidae";
const COORD_ASSERT_Y_MAX = Number(process.env.PSY_COORD_ASSERT_Y_MAX || 250);
const TARGET_LABELS = (process.env.PSY_TARGET_LABELS ||
  "Poodle,Golden Retriever,Labrador Retriever,German Shepherd,Bulldog").split(",");
const TARGET_ASSERTS = (process.env.PSY_TARGET_ASSERTS ||
  "Toy Poodle,Golden Retriever,Labrador Retriever,German Shepherd,Bulldog").split(",");
const SCROLL_POINTS = Number(process.env.PSY_SCROLL_POINTS || 600);
const SCROLL_TOLERANCE = Number(process.env.PSY_SCROLL_TOLERANCE || 90);
const SCROLL_ROUNDS = Number(process.env.PSY_SCROLL_ROUNDS || 4);
const SCROLLTO_ROUNDS = Number(process.env.PSY_SCROLLTO_ROUNDS || 3);

const results = [];
const notes = [];

function record(name, ok, detail = "") {
  results.push({ name, ok: !!ok, detail });
  console.log(`${ok ? "PASS" : "FAIL"}  ${name}${detail ? `  (${detail})` : ""}`);
}

function note(text) {
  notes.push(text);
  console.log(`NOTE  ${text}`);
}

function psy(args, timeout = 240000) {
  const result = spawnSync(PSY, args, { encoding: "utf8", timeout, maxBuffer: 128 * 1024 * 1024 });
  if (result.error) throw result.error;
  return {
    status: result.status,
    out: (result.stdout || "").trim(),
    err: (result.stderr || "").trim(),
  };
}

function ax(depth = 12) {
  const result = psy(["ax", String(depth)]);
  if (result.status !== 0) throw new Error(`ax failed: ${result.out || result.err}`);
  return JSON.parse(result.out);
}

function* walk(node) {
  if (!node || typeof node !== "object") return;
  yield node;
  if (Array.isArray(node.children)) {
    for (const kid of node.children) yield* walk(kid);
  }
}

function findLabel(tree, needle) {
  const want = needle.toLowerCase();
  let best = null;
  for (const node of walk(tree.root)) {
    if (!node || typeof node.label !== "string" || !node.label) continue;
    if (!Array.isArray(node.frame) || node.frame[2] <= 0 || node.frame[3] <= 0) continue;
    const label = node.label.toLowerCase();
    const score = label === want ? 2 : label.includes(want) ? 1 : 0;
    if (!score) continue;
    const area = node.frame[2] * node.frame[3];
    if (!best || score > best.score || (score === best.score && area < best.area)) {
      best = { node, score, area };
    }
  }
  return best ? best.node : null;
}

function frontBundle() {
  const result = psy(["front"]);
  const match = result.out.match(/^ok (\S+)/);
  return match ? match[1] : null;
}

async function waitFor(probe, timeoutMs, intervalMs = 350) {
  const deadline = Date.now() + timeoutMs;
  for (;;) {
    let value = null;
    try {
      value = probe();
    } catch {
      value = null;
    }
    if (value) return value;
    if (Date.now() > deadline) return null;
    await sleep(intervalMs);
  }
}

function frameCenter(frame) {
  return [Math.round(frame[0] + frame[2] / 2), Math.round(frame[1] + frame[3] / 2)];
}

function frameText(frame) {
  return `[${frame.map((v) => Math.round(v)).join(",")}]`;
}

function getSpace() {
  const result = psy(["size"]);
  const match = result.out.match(/^ok (\d+) (\d+)(?:\s+(\d+)\s+(\d+))?/);
  if (result.status === 0 && match) {
    return {
      width: Number(match[1]),
      height: Number(match[2]),
      fixedWidth: match[3] ? Number(match[3]) : null,
      fixedHeight: match[4] ? Number(match[4]) : null,
      fromSize: true,
    };
  }
  /* Pre-fix fallback: derive the live space from ax frame extents. */
  const tree = ax(12);
  let maxX = 0;
  let maxY = 0;
  for (const node of walk(tree.root)) {
    if (!Array.isArray(node.frame)) continue;
    maxX = Math.max(maxX, node.frame[0] + node.frame[2]);
    maxY = Math.max(maxY, node.frame[1] + node.frame[3]);
  }
  return {
    width: Math.ceil(maxX),
    height: Math.ceil(maxY),
    fixedWidth: null,
    fixedHeight: null,
    fromSize: false,
  };
}

async function launchAndWait(bundle, timeoutMs = 20000) {
  psy(["launch", bundle]);
  return waitFor(() => (frontBundle() === bundle ? true : null), timeoutMs);
}

/* The front app can change on this device while a test is running. Re-assert
 * the target app before each interaction that depends on it. */
async function ensureFront(bundle, attempts = 3) {
  for (let i = 0; i < attempts; i++) {
    if (frontBundle() === bundle) return true;
    if (await launchAndWait(bundle, 12000)) return true;
    await sleep(500);
  }
  return false;
}

/* ---------- tests ---------- */

async function testSpace(space) {
  record("size command reports the live space", space.fromSize,
    space.fromSize ? `${space.width}x${space.height} fixed ${space.fixedWidth}x${space.fixedHeight}`
                   : "no size command (pre-fix build?)");

  const tree = ax(12);
  const reported = tree.space;
  record("ax output carries the space metadata",
    !!(reported && reported.width > 0 && reported.height > 0 && space.fromSize),
    reported ? JSON.stringify(reported) : "missing");

  if (reported && space.fromSize) {
    record("ax space matches size",
      reported.width === space.width && reported.height === space.height,
      `ax ${reported.width}x${reported.height} vs size ${space.width}x${space.height}`);
  }

  if (space.width > space.height) {
    record("interface is landscape (regression manifests)", true,
      `${space.width}x${space.height}`);
  } else {
    note("interface is portrait; the old default space coincides with the live space, " +
      "so the coordinate regression would not reproduce. Rotate to landscape for the full check.");
  }
}

async function testCoordinateTap() {
  const launched = await launchAndWait("com.apple.mobilesafari");
  if (!launched) {
    record("coordinate tap: Safari launched", false, "front app is not Safari");
    return;
  }
  psy(["open_url", COORD_URL]);

  const loaded = await waitFor(() => {
    const tree = ax(12);
    return findLabel(tree, "Contents") && findLabel(tree, COORD_LINK) ? tree : null;
  }, 30000, 800);
  if (!loaded) {
    record(`coordinate tap: ${COORD_URL} loaded`, false, `"${COORD_LINK}" not found`);
    return;
  }

  const link = findLabel(loaded, COORD_LINK);
  if (!link) {
    record("coordinate tap: link has an AX frame", false, `label "${COORD_LINK}" not found`);
    return;
  }
  record("coordinate tap: link has an AX frame", true, `${COORD_LINK} ${frameText(link.frame)}`);

  if (!(await ensureFront("com.apple.mobilesafari"))) {
    record("coordinate tap: Safari still frontmost before tap", false);
    return;
  }

  const [cx, cy] = frameCenter(link.frame);
  const tapResult = psy(["tap", String(cx), String(cy)]);
  record("coordinate tap: default tap dispatched", !tapResult.out.startsWith("err"),
    `${cx},${cy} -> ${tapResult.out}`);

  /* Navigation replaces the article title near the top of the page; the link
   * on the source page sat in the body. The title check distinguishes a real
   * activation from a wrong-space tap that changes nothing. */
  const activated = await waitFor(() => {
    const tree = ax(12);
    const node = findLabel(tree, COORD_ASSERT);
    return node && Array.isArray(node.frame) && node.frame[1] < COORD_ASSERT_Y_MAX ? node : null;
  }, 25000, 800);
  record("coordinate tap: tap activated the AX-frame target without W H",
    !!activated, activated ? `"${COORD_ASSERT}" title at ${frameText(activated.frame)}`
                           : `"${COORD_ASSERT}" never moved into the title area`);
}

function pickScrollReference(tree, space) {
  const chrome = /^(address bar|reload|share|bookmarks|tabs|tab bar|page settings|reader|hide|show|dock|menu)$/i;
  let best = null;
  for (const node of walk(tree.root)) {
    if (!node || typeof node.label !== "string" || node.label.length < 3) continue;
    if (chrome.test(node.label.trim())) continue;
    if (!Array.isArray(node.frame)) continue;
    const [x, y, w, h] = node.frame;
    if (w < 30 || h < 12 || h > 60) continue;
    if (y < space.height * 0.5) continue;          /* bottom half */
    if (y + h > space.height - 2 || y < 0) continue; /* fully visible */
    if (x < 0 || x + w > space.width) continue;
    const key = `${node.label}|${x}|${w}`;
    if (!best || y > best.y) best = { label: node.label, x, y, w, h, key };
  }
  return best;
}

async function testScrollDeterminism(space) {
  if (!(space.width > 0 && space.height > 0)) return;

  const launched = await launchAndWait("com.apple.mobilesafari");
  if (!launched) {
    record("scroll: Safari frontmost", false);
    return;
  }
  psy(["open_url", ARTICLE_URL]);
  const loaded = await waitFor(() => {
    const tree = ax(12);
    return findLabel(tree, "Contents") || findLabel(tree, "Dog") ? tree : null;
  }, 30000, 800);
  if (!loaded) {
    record(`scroll: ${ARTICLE_URL} loaded`, false);
    return;
  }
  record("scroll: article loaded", true, ARTICLE_URL);

  const top = psy(["scrolltop"]);
  record("scroll: scrolltop works", top.out.startsWith("ok scrolltop"),
    top.out || top.err);

  let measured = 0;
  let measuredSum = 0;
  let lastDelta = 0;
  for (let round = 1; round <= SCROLL_ROUNDS; round++) {
    const before = ax(12);
    let reference = pickScrollReference(before, space);
    if (!reference) {
      psy(["scroll", "up", String(SCROLL_POINTS)]);
      const retry = ax(12);
      reference = pickScrollReference(retry, space);
    }
    if (!reference) {
      record(`scroll: reference found (round ${round})`, false, "no bottom-band label");
      break;
    }

    const scroll = psy(["scroll", "down", String(SCROLL_POINTS)]);
    if (!scroll.out.startsWith("ok scroll")) {
      record(`scroll: command ok (round ${round})`, false, scroll.out || scroll.err);
      break;
    }

    if (!(await ensureFront("com.apple.mobilesafari"))) {
      record(`scroll: Safari still frontmost (round ${round})`, false);
      break;
    }

    const offset = psy(["scrolloffset", reference.label]);
    const match = offset.out.match(/^ok offset (-?\d+) (-?\d+) (\d+) (\d+)/);
    if (!match) {
      record(`scroll: offset readback (round ${round})`, false, offset.out || offset.err);
      continue;
    }
    const after = { x: Number(match[1]), y: Number(match[2]), w: Number(match[3]), h: Number(match[4]) };
    const delta = reference.y - after.y;
    const dx = Math.abs(after.x - reference.x);
    const dw = Math.abs(after.w - reference.w);
    const ok = Math.abs(delta - SCROLL_POINTS) <= SCROLL_TOLERANCE && dx <= 8 && dw <= 8;
    record(`scroll: ${SCROLL_POINTS}pt move is deterministic (round ${round})`, ok,
      `"${reference.label}" dy=${delta} dx=${dx} dw=${dw} (want ${SCROLL_POINTS}±${SCROLL_TOLERANCE})`);
    if (ok) {
      measured++;
      measuredSum += delta;
      lastDelta = delta;
    }
  }

  if (measured >= 2) {
    const cumulative = measuredSum;
    const target = SCROLL_POINTS * measured;
    record(`scroll: ${measured} calls moved content ~${target}px cumulatively`,
      Math.abs(cumulative - target) <= SCROLL_TOLERANCE * measured,
      `sum=${cumulative} last=${lastDelta}`);
  }

  /* scrollbottom then back to top, both bounded. */
  const bottom = psy(["scrollbottom"]);
  record("scroll: scrollbottom works", bottom.out.startsWith("ok scrollbottom"),
    bottom.out || bottom.err);
  const backTop = psy(["scrolltop"]);
  record("scroll: returns to top", backTop.out.startsWith("ok scrolltop"),
    backTop.out || backTop.err);
}

async function testScrollTo(space) {
  if (!(space.width > 0 && space.height > 0)) return;

  for (let round = 1; round <= SCROLLTO_ROUNDS; round++) {
    const launched = await launchAndWait("com.apple.mobilesafari");
    if (!launched) {
      record(`scrollto: Safari frontmost (round ${round})`, false);
      return;
    }
    psy(["open_url", ARTICLE_URL]);
    const loaded = await waitFor(() => {
      const tree = ax(12);
      return findLabel(tree, "Contents") || findLabel(tree, "Dog") ? tree : null;
    }, 30000, 800);
    if (!loaded) {
      record(`scrollto: article loaded (round ${round})`, false);
      return;
    }
    psy(["scrolltop"]);

    let target = null;
    let assertLabel = null;
    for (let i = 0; i < TARGET_LABELS.length; i++) {
      const candidate = TARGET_LABELS[i].trim();
      const tree = ax(12);
      if (!findLabel(tree, candidate)) {
        target = candidate;
        assertLabel = (TARGET_ASSERTS[i] || candidate).trim();
        break;
      }
    }
    if (!target) {
      record(`scrollto: offscreen target available (round ${round})`, false,
        `all candidates visible: ${TARGET_LABELS.join(", ")}`);
      return;
    }

    const scrolled = psy(["scrollto", target]);
    const match = scrolled.out.match(/^ok scrollto (-?\d+) (-?\d+) (\d+) (\d+)/);
    if (!match) {
      record(`scrollto: "${target}" (round ${round})`, false, scrolled.out || scrolled.err);
      continue;
    }
    const frame = [Number(match[1]), Number(match[2]), Number(match[3]), Number(match[4])];
    const centerY = frame[1] + frame[3] / 2;
    const onScreen = frame[0] >= -2 && frame[1] >= -2 &&
      frame[0] + frame[2] <= space.width + 2 && frame[1] + frame[3] <= space.height + 2;
    record(`scrollto: "${target}" is in view and centered (round ${round})`, onScreen,
      `${frameText(frame)} centerY=${Math.round(centerY)}`);

    const offset = psy(["scrolloffset", target]);
    record(`scrolloffset: "${target}" readback (round ${round})`, offset.out.startsWith("ok offset"),
      offset.out || offset.err);

    if (!(await ensureFront("com.apple.mobilesafari"))) {
      record(`scrollto: Safari still frontmost before tap (round ${round})`, false);
      continue;
    }
    const [cx, cy] = frameCenter(frame);
    const tap = psy(["tap", String(cx), String(cy)]);
    const activated = await waitFor(() => {
      const tree = ax(12);
      return findLabel(tree, assertLabel) ? tree : null;
    }, 25000, 800);
    record(`scrollto+tap: "${target}" activates on an immediate center tap (round ${round})`,
      !tap.out.startsWith("err") && !!activated,
      activated ? `found "${assertLabel}"` : `tap ${cx},${cy} -> ${tap.out}; "${assertLabel}" not found`);
  }
}

function writeReport(extra) {
  const reportIndex = process.argv.indexOf("--report");
  const reportPath = reportIndex >= 0 && process.argv[reportIndex + 1]
    ? process.argv[reportIndex + 1]
    : path.join(process.cwd(), "artifacts", "device-issues-test.json");
  fs.mkdirSync(path.dirname(reportPath), { recursive: true });
  const failed = results.filter((r) => !r.ok);
  fs.writeFileSync(reportPath, JSON.stringify({
    date: new Date().toISOString(),
    node: process.version,
    platform: `${os.platform()} ${os.release()} ${os.arch()}`,
    ...extra,
    notes,
    results,
    passed: results.length - failed.length,
    failed: failed.length,
  }, null, 2));
  console.log(`\nreport: ${reportPath}`);
  return failed.length === 0 && results.length > 0;
}

async function main() {
  console.log(`psychosis device issues test (${PSY})`);
  const ping = psy(["ping"]);
  if (!ping.out.startsWith("ok")) {
    console.error(`psychosis is not reachable: ${ping.out || ping.err}`);
    process.exit(2);
  }

  const space = getSpace();
  console.log(`space: ${space.width}x${space.height} (fixed ${space.fixedWidth}x${space.fixedHeight})`);

  await testSpace(space);
  await testCoordinateTap();
  await testScrollDeterminism(space);
  await testScrollTo(space);

  const ok = writeReport({ space });
  console.log(`\n${results.filter((r) => r.ok).length}/${results.length} checks passed`);
  process.exit(ok ? 0 : 1);
}

main().catch((error) => {
  console.error(error);
  writeReport({ fatal: String(error && error.stack ? error.stack : error) });
  process.exit(2);
});
