// "Fast with Tov", the singalong: the song, its words lit as they're sung, and a stage that acts
// them out in time. The stage is one canvas (drawn from the song's clock every frame, so seeking
// and pausing just work); the lyrics are text over it. The landing page loads this file when
// someone asks to sing (page.tov) and calls window.tovSingalong().
(() => {
"use strict";

// Each line: its section, and its words, each with the second it starts and ends (the lyrics
// force-aligned to the song's separated vocals, then snapped to where the voice starts).
const LYRICS = [["intro",[["Oi!",0.105,0.798],["Agents!",1.234,2.045],["Pints",2.98,3.447],["up!",3.447,3.647]]],["chorus",[["So",9.339,9.813],["it's",9.813,10.072],["Tov!",10.072,10.925],["Tov!",10.925,11.663],["Faster",11.663,12.267],["than",12.267,12.617],["Rust!",12.617,13.165]]],["chorus",[["Written",13.325,13.918],["like",13.918,14.387],["TypeScript,",14.387,15.061],["it's",15.061,15.255],["Tov",15.255,15.689],["or",15.689,16.123],["bust!",16.123,16.557]]],["chorus",[["Your",16.792,17.041],["agent",17.041,17.742],["writes",17.742,18.119],["it",18.119,18.498],["and",18.498,18.668],["Tov",18.668,19.092],["puts",19.092,19.371],["it",19.371,19.516],["right,",19.516,19.83]]],["chorus",[["Ship",19.83,20.364],["one",20.364,20.833],["little",20.833,21.232],["binary,",21.232,22.105],["down",22.105,22.367],["the",22.367,22.539],["pub",22.539,22.823],["tonight!",22.823,23.756]]],["verse1",[["Me",26.994,27.27],["agent's",27.27,27.907],["a",27.907,27.967],["grafter,",27.967,28.711],["it",28.711,28.874],["codes",28.874,29.343],["all",29.343,29.711],["night,",29.711,30.007]]],["verse1",[["But",30.311,30.441],["it",30.441,30.605],["guesses",30.605,31.304],["a",31.304,31.364],["lot",31.364,31.723],["and",31.888,32.012],["it",32.012,32.202],["never",32.202,32.596],["gets",32.596,32.895],["it",32.895,33.04],["right,",33.04,33.454]]],["verse1",[["So",33.54,33.968],["Tov",33.968,34.387],["tells",34.387,34.672],["it",34.672,34.891],["straight",34.891,35.24],["what",35.24,35.415],["it",35.415,35.604],["needs",35.604,36.372],["to",36.372,36.537],["do,",36.537,36.851]]],["verse1",[["Here's",36.956,37.24],["the",37.24,37.393],["error,",37.393,37.799],["here's",37.799,38.078],["the",38.078,38.258],["fix,",38.258,38.662],["and",38.662,38.941],["the",38.941,39.106],["build",39.106,39.52],["goes",39.52,39.939],["through!",39.939,40.393]]],["chorus",[["So",40.393,40.553],["it's",40.553,40.777],["Tov!",40.777,41.61],["Tov!",41.61,42.309],["Faster",42.309,42.957],["than",42.957,43.334],["Rust!",43.334,43.815]]],["chorus",[["Written",44.015,44.494],["like",44.494,45.033],["TypeScript,",45.033,45.746],["it's",45.746,45.931],["Tov",45.931,46.36],["or",46.36,46.804],["bust!",46.804,47.223]]],["chorus",[["Your",47.412,47.715],["agent",47.715,48.395],["writes",48.395,48.759],["it",48.759,49.143],["and",49.143,49.303],["Tov",49.303,49.742],["puts",49.742,50.031],["it",50.031,50.156],["right,",50.156,50.465]]],["chorus",[["Ship",50.465,51.009],["one",51.009,51.416],["little",51.416,51.867],["binary,",51.867,52.71],["down",52.71,52.99],["the",52.99,53.154],["pub",53.154,53.429],["tonight!",53.429,53.932]]],["verse2",[["No",53.932,54.436],["any,",54.436,55.025],["no",55.025,55.299],["null,",55.299,55.898],["no",55.898,56.132],["double",56.132,56.497],["equals",56.497,56.985],["sign,",56.985,57.39]]],["verse2",[["Put",57.39,57.519],["a",57.519,57.714],["try",57.714,58.317],["on",58.317,58.517],["the",58.517,58.652],["call",58.652,58.997],["and",58.997,59.138],["it'll",59.138,59.438],["all",59.438,59.969],["be",59.969,60.388],["fine,",60.388,60.807]]],["verse2",[["Forty-two",60.807,61.665],["milliseconds",61.665,62.533],["and",62.533,62.738],["the",62.738,62.942],["build",62.942,63.356],["is",63.356,63.785],["done,",63.785,64.139]]],["verse2",[["Poor",64.184,64.459],["old",64.459,64.618],["Rust's",64.618,65.062],["still",65.062,65.312],["compiling,",65.312,66.135],["it's",66.135,66.344],["missed",66.344,66.594],["all",66.594,67.003],["the",67.003,67.172],["fun!",67.172,67.926]]],["bridge",[["Tap,",67.97,68.449],["tap,",68.449,68.859],["tap",68.859,69.302],["go",69.302,69.572],["the",69.572,69.82],["agent's",69.82,70.55],["keys,",70.55,71.338]]],["bridge",[["Bleep,",71.418,71.847],["bleep,",71.847,72.276],["bleep,",72.276,72.63],["check",72.63,72.974],["the",72.974,73.104],["JSON",73.104,73.967],["please,",73.967,74.73]]],["bridge",[["Five!",74.8,75.663],["Four!",75.663,76.481],["Three!",76.481,76.905],["Two!",76.905,77.402],["One!",77.402,78.222]]],["bridge",[["(DING!)",78.222,79.694],["That's",79.694,80.133],["the",80.133,80.302],["build",80.302,80.722],["done!",80.722,81.176]]],["final",[["So",81.18,81.362],["it's",81.362,81.585],["Tov!",81.585,82.453],["Tov!",82.453,83.166],["Faster",83.166,83.703],["than",83.703,84.103],["Rust!",84.103,84.817]]],["final",[["Written",84.817,85.436],["like",85.436,85.815],["TypeScript,",85.815,86.528],["it's",86.528,86.708],["Tov",86.708,87.142],["or",87.142,87.566],["bust!",87.566,88.205]]],["final",[["Your",88.205,88.504],["agent",88.504,89.204],["writes",89.204,89.584],["it",89.584,89.911],["and",89.911,90.08],["Tov",90.08,90.509],["puts",90.509,90.804],["it",90.804,90.945],["right,",90.945,91.387]]],["final",[["Ship",91.387,91.781],["one",91.781,92.185],["little",92.185,92.634],["binary,",92.634,93.512],["down",93.512,93.767],["the",93.767,93.927],["pub",93.927,94.206],["tonight!",94.206,95.124]]],["outro",[["Tov!",95.125,96.002],["Tov!",96.002,96.73],["Faster",96.73,97.329],["than",97.329,97.666],["Rust!",97.666,98.536]]],["outro",[["Tov!",98.536,99.389],["Tov!",99.389,100.232],["Tov",100.232,100.716],["or",100.716,101.145],["bust!",101.145,101.953]]],["outro",[["Curl",101.953,102.327],["it,",102.327,102.821],["pipe",102.821,103.106],["it,",103.106,103.639],["off",103.639,104.019],["you",104.019,104.522],["go,",104.522,105.316]]],["outro",[["Tov",105.316,106.164],["dot",106.164,107.022],["S-H...",107.022,107.948],["OI!",107.948,108.868]]]];
// The song's loudness, 30 times a second: bass, overall and treble, one byte each (base64).
const ENV = "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";
const BPM = 141, BEAT = 60 / BPM, BEAT0 = 0.346;
const SONG = "/fast-with-tov.mp3";
const INSTALL = "curl -fsSL https://tov.sh/install.sh | sh";

const C = {
  amber: "#FFC233", amberHi: "#FFE45C", foam: "#FFF6E0", ink: "#1E1033", rust: "#F0602A",
  green: "#5CFF8F", red: "#FF4D5E", cyan: "#26E8FF", pink: "#FF3D9A", violet: "#A35CFF",
  lime: "#C8FF3D", yellow: "#FFEA3D", blue: "#3D6BFF", orange: "#FF7A1A",
};
const FONT = `"Schibsted Grotesk", ui-sans-serif, system-ui, sans-serif`;
const MONO = `ui-monospace, "SF Mono", Menlo, Consolas, monospace`;
const calm = matchMedia("(prefers-reduced-motion: reduce)").matches;

// ---------------------------------------------------------------- small maths

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const lerp = (a, b, k) => a + (b - a) * k;
const out3 = k => 1 - Math.pow(1 - clamp(k, 0, 1), 3);
const inOut = k => (k = clamp(k, 0, 1), k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
const back = k => { k = clamp(k, 0, 1) - 1; return 1 + k * k * (2.7 * k + 1.7); };
const TAU = Math.PI * 2;
function hash(n) { n = Math.sin(n * 127.1 + 311.7) * 43758.5453; return n - Math.floor(n); }
function hex(h) { const n = parseInt(h.slice(1), 16); return [n >> 16, (n >> 8) & 255, n & 255]; }
function mix(a, b, k) { return `rgb(${Math.round(lerp(a[0], b[0], k))},${Math.round(lerp(a[1], b[1], k))},${Math.round(lerp(a[2], b[2], k))})`; }

const envBytes = Uint8Array.from(atob(ENV), ch => ch.charCodeAt(0));
function env(t, k) {
  const f = t * 30, i = Math.floor(f), n = envBytes.length / 3 - 1;
  if (i < 0 || i >= n) return 0;
  const a = envBytes[i * 3 + k], b = envBytes[i * 3 + 3 + k];
  return (a + (b - a) * (f - i)) / 255;
}

// ---------------------------------------------------------------- the song as data

const lines = LYRICS.map(([sec, ws], i) => {
  const words = ws.map(([text, s, e], j) => ({ text, s, e, j, el: null, lit: false }));
  return { i, sec, words, s: words[0].s, e: words[words.length - 1].e, el: null };
});
// A line takes the stage a little before its first word (once the line before has mostly sung
// its last), and leaves when the next one comes, or a second and a half after it ends.
lines.forEach((l, i) => {
  const before = lines[i - 1];
  if (!before) { l.show = -1; return; }
  const last = before.words[before.words.length - 1];
  const after = last.s + 0.6 * (last.e - last.s);
  l.show = Math.max(after, Math.min(l.s - 0.6, l.s - 0.05));
  if (l.show > l.s - 0.05) l.show = Math.max(last.s + 0.1, l.s - 0.05);
});
lines.forEach((l, i) => { l.hide = i + 1 < lines.length ? Math.min(lines[i + 1].show, l.e + 1.5) : l.e + 4; });
const W = (li, wi) => lines[li].words[wi];
const L = li => lines[li];
// when the line after this one starts (a scene for this line is gone by then)
const until = li => (li + 1 < lines.length ? lines[li + 1].s : lines[li].e + 2);

const NAMES = {
  intro: "Oi oi!", chorus: "Chorus! Everybody!", verse1: "Verse one", verse2: "Verse two",
  bridge: "Arcade mode", final: "Last chorus, louder!", outro: "All together now",
};
// each section's sky (top, middle, bottom), its sticker colour, and what's drawn behind the stage
const THEMES = {
  intro: { sky: ["#2B0B6E", "#B5179E", "#FF4D6D"], tag: C.yellow, bg: "pub" },
  chorus: { sky: ["#FF2E7E", "#FF6A2B", "#FFB627"], tag: C.cyan, bg: "burst" },
  verse1: { sky: ["#07073A", "#2A1A8F", "#7B2FF7"], tag: C.lime, bg: "night" },
  verse2: { sky: ["#002E3B", "#00877F", "#00D1A0"], tag: C.pink, bg: "grid" },
  bridge: { sky: ["#0D0026", "#4B00A8", "#FF2EB5"], tag: C.cyan, bg: "arcade" },
  final: { sky: ["#FF006E", "#FF4D2E", "#FFC300"], tag: C.lime, bg: "burst" },
  outro: { sky: ["#2D4BFF", "#B026FF", "#FF2E7E"], tag: C.yellow, bg: "burst" },
};

const sections = [];
for (const l of lines) {
  const last = sections[sections.length - 1];
  if (!last || last.kind !== l.sec) sections.push({ kind: l.sec, s: l.s, e: l.e, lines: [l] });
  else { last.lines.push(l); last.e = l.e; }
}
// a section's look arrives a little before its first word (or halfway through the gap before it)
sections.forEach((s, i) => {
  if (i === 0) { s.at = 0; return; }
  const last = sections[i - 1].lines[sections[i - 1].lines.length - 1];
  s.at = Math.min(s.s - 0.05, Math.max(last.words[last.words.length - 1].s + 0.3, s.s - 1.4));
});
function sectionAt(t) {
  let i = 0;
  while (i + 1 < sections.length && sections[i + 1].at <= t) i++;
  return i;
}

// ---------------------------------------------------------------- the page around it

const css = `
#ts { position: fixed; inset: 0; z-index: 1000; background: #1E1033; color: #fff; font-family: ${FONT}; overflow: hidden; touch-action: manipulation; -webkit-user-select: none; user-select: none; -webkit-tap-highlight-color: transparent; contain: strict; }
#ts[hidden] { display: none; }
#ts button { font: inherit; color: inherit; cursor: pointer; border: 0; background: none; }
/* the sky (two layers, crossfading between sections), the stage, then shading and flashes */
#ts .ts-sky { position: absolute; inset: 0; opacity: 0; visibility: hidden; transition: opacity 1.1s ease, visibility 0s 1.1s; }
#ts .ts-sky.on { opacity: 1; visibility: visible; transition: opacity 1.1s ease; }
#ts .ts-cv { position: absolute; inset: 0; width: 100%; height: 100%; display: block; }
#ts .ts-scan { position: absolute; inset: 0; pointer-events: none; display: none; background: repeating-linear-gradient(to bottom, rgba(0,0,0,.16) 0 2px, transparent 2px 4px); }
#ts.is-arcade .ts-scan { display: block; }
#ts .ts-flash { position: absolute; inset: 0; pointer-events: none; opacity: 0; display: none; }
#ts .ts-top { position: absolute; top: 0; left: 0; right: 0; display: flex; align-items: center; justify-content: space-between; gap: 12px; padding: max(14px, env(safe-area-inset-top)) max(18px, env(safe-area-inset-right)) 14px max(18px, env(safe-area-inset-left)); z-index: 3; pointer-events: none; }
#ts .ts-top > * { pointer-events: auto; }
#ts .ts-name { display: flex; align-items: center; gap: 10px; font-weight: 900; letter-spacing: -.02em; font-size: 1.125rem; line-height: 1.05; text-shadow: 0 2px 0 rgba(30,16,51,.5); }
#ts .ts-name small { display: block; font-weight: 600; font-size: .8125rem; letter-spacing: 0; opacity: .8; }
#ts .ts-name svg { width: 44px; height: auto; filter: drop-shadow(0 2px 0 rgba(30,16,51,.4)); }
#ts .ts-x { width: 46px; height: 46px; border-radius: 14px; background: ${C.ink} !important; color: #fff; display: grid; place-items: center; box-shadow: 0 4px 0 rgba(0,0,0,.3); transition: transform .15s; }
#ts .ts-x:hover { transform: rotate(8deg) scale(1.06); }
#ts .ts-x svg { width: 18px; height: 18px; }
/* the section's sticker */
#ts .ts-sec { position: absolute; top: max(78px, calc(env(safe-area-inset-top) + 64px)); left: 50%; z-index: 2; font-size: 1.0625rem; font-weight: 900; letter-spacing: -.01em; padding: 7px 16px 8px; border-radius: 12px; color: ${C.ink}; background: var(--tag, ${C.yellow}); box-shadow: 0 4px 0 rgba(30,16,51,.45); white-space: nowrap; transform: translateX(-50%) rotate(-3deg); transition: opacity .3s; }
#ts .ts-sec.swap { animation: ts-sticker .6s cubic-bezier(.3,1.7,.5,1); }
#ts .ts-hint { position: absolute; left: 50%; top: max(128px, calc(env(safe-area-inset-top) + 114px)); transform: translateX(-50%) rotate(1.5deg); z-index: 2; font-size: .9375rem; font-weight: 700; padding: 9px 14px; border-radius: 12px; background: ${C.ink}; box-shadow: 0 4px 0 rgba(0,0,0,.25); white-space: nowrap; opacity: 0; transition: opacity .5s; pointer-events: none; }
#ts .ts-hint.show { opacity: 1; }
#ts .ts-hint b { color: ${C.yellow}; }
/* the lyrics: the line being sung, the next one under it */
#ts .ts-lyrics { position: absolute; left: max(14px, env(safe-area-inset-left)); right: max(14px, env(safe-area-inset-right)); bottom: calc(92px + env(safe-area-inset-bottom)); z-index: 2; pointer-events: none; height: 0; }
#ts .ts-l { position: absolute; left: 0; right: 0; bottom: 0; margin: 0 auto; max-width: 19em; text-align: center; font-weight: 900; font-size: clamp(1.5rem, min(5.2vw, 8vh), 6.5rem); line-height: 1.12; letter-spacing: -.035em; text-wrap: balance; transform-origin: 50% 100%; opacity: 0; transform: translateY(.6em) scale(.5); visibility: hidden; transition: transform .5s cubic-bezier(.2,.9,.2,1.15), opacity .35s ease, visibility 0s .5s; text-shadow: 0 .07em 0 rgba(30,16,51,.6); }
#ts .ts-l.is-cur { opacity: 1; visibility: visible; transform: translateY(calc(var(--nh, 0px) * -1 - .25em)) scale(1); transition-delay: 0s; }
#ts .ts-l.is-next { opacity: .62; visibility: visible; transform: scale(.5); transition-delay: 0s; }
#ts .ts-l.is-prev { opacity: 0; visibility: visible; transform: translateY(calc(var(--nh, 0px) * -1 - var(--h, 1em) - .4em)) scale(.62) rotate(-2deg); transition-delay: 0s; }
#ts .ts-w { position: relative; display: inline-block; color: rgba(255,255,255,.42); white-space: pre; transform-origin: 50% 80%; }
#ts .ts-w > i { position: absolute; left: 0; top: 0; font-style: normal; color: var(--c, #fff); clip-path: inset(-30% calc(100% - var(--p, 0) * 100%) -30% -10%); }
#ts .ts-w.is-on { animation: ts-pop .32s cubic-bezier(.3,1.8,.5,1) both; }
/* words with a voice of their own */
#ts .ts-w.ts-tov { --c: ${C.yellow}; margin: 0 .16em; }
#ts .ts-w.ts-tov.is-on { animation: ts-boing .6s cubic-bezier(.3,1.8,.5,1) both; }
#ts .ts-w.ts-tov.is-done { color: ${C.yellow}; }
#ts .ts-w.ts-shout { --c: ${C.lime}; margin: 0 .14em; }
#ts .ts-w.ts-shout.is-on { animation: ts-shout .5s cubic-bezier(.3,1.8,.5,1) both; }
#ts .ts-w.ts-rust { --c: ${C.rust}; }
#ts .ts-w.ts-rust.is-on { animation: ts-rusty .5s ease-in-out both; }
#ts .ts-w.ts-fast { --c: ${C.cyan}; }
#ts .ts-w.ts-fast.is-on { animation: ts-zoom .45s cubic-bezier(.2,1.6,.4,1) both; }
#ts .ts-w.ts-code { font-family: ${MONO}; font-weight: 800; letter-spacing: -.05em; font-size: .88em; --c: ${C.cyan}; }
#ts .ts-w.ts-no { --c: ${C.cyan}; }
#ts .ts-w.ts-no::after { content: ""; position: absolute; left: -6%; right: -6%; top: 52%; height: .12em; border-radius: .06em; background: ${C.red}; transform: scaleX(0) rotate(-6deg); transform-origin: 0 50%; transition: transform .2s cubic-bezier(.3,1.6,.5,1); }
#ts .ts-w.ts-no.is-done::after { transform: scaleX(1) rotate(-6deg); }
#ts .ts-w.ts-good { --c: ${C.green}; }
#ts .ts-w.ts-good.is-on { animation: ts-jelly .5s cubic-bezier(.3,1.6,.5,1) both; }
#ts .ts-w.ts-bad { --c: ${C.red}; }
#ts .ts-w.ts-bad.is-on { animation: ts-shake .4s linear both; }
#ts .ts-w.ts-small.is-on { animation: ts-shrink .4s cubic-bezier(.3,1.6,.5,1) both; }
#ts .ts-w.ts-beer { --c: ${C.amber}; }
#ts .ts-w.ts-night { --c: #C9B8FF; }
#ts .ts-w.ts-beep { --c: ${C.pink}; }
#ts .ts-w.ts-beep.is-on { animation: ts-press .3s cubic-bezier(.3,1.6,.5,1) both; }
#ts .ts-w.ts-big { --c: ${C.pink}; margin: 0 .14em; }
#ts .ts-w.ts-big.is-on { animation: ts-shout .5s cubic-bezier(.3,1.8,.5,1) both; }
@keyframes ts-pop { 0% { transform: none; } 40% { transform: translateY(-.12em) scale(1.08); } 100% { transform: translateY(-.04em); } }
@keyframes ts-boing { 0% { transform: none; } 30% { transform: scale(1.45) rotate(-8deg); } 55% { transform: scale(1.1) rotate(5deg); } 75% { transform: scale(1.22) rotate(-3deg); } 100% { transform: scale(1.16) rotate(-2deg); } }
@keyframes ts-shout { 0% { transform: none; } 25% { transform: scale(1.28) rotate(4deg); } 45% { transform: scale(1.12) translateX(-.04em) rotate(-3deg); } 65% { transform: scale(1.18) translateX(.04em) rotate(2deg); } 100% { transform: scale(1.12) rotate(0); } }
@keyframes ts-rusty { 0%, 100% { transform: none; } 20% { transform: rotate(-6deg) translateY(.04em); } 40% { transform: rotate(5deg); } 60% { transform: rotate(-4deg) translateY(.06em); } 80% { transform: rotate(3deg); } }
@keyframes ts-zoom { 0% { transform: none; } 40% { transform: skewX(-20deg) translateX(.05em); } 100% { transform: skewX(-10deg); } }
@keyframes ts-jelly { 0% { transform: none; } 30% { transform: scale(1.18, .8); } 55% { transform: scale(.92, 1.15); } 75% { transform: scale(1.04, .96); } 100% { transform: none; } }
@keyframes ts-shake { 0%, 100% { transform: none; } 20% { transform: translateX(-.06em) rotate(-2deg); } 40% { transform: translateX(.06em) rotate(2deg); } 60% { transform: translateX(-.04em); } 80% { transform: translateX(.04em); } }
@keyframes ts-shrink { 0% { transform: none; } 50% { transform: scale(.62); } 100% { transform: scale(.78); } }
@keyframes ts-press { 0% { transform: none; } 35% { transform: translateY(.14em) scale(1.1, .82); } 100% { transform: translateY(-.03em); } }
@keyframes ts-sticker { 0% { opacity: 0; transform: translateX(-50%) scale(.4) rotate(-14deg); } 100% { opacity: 1; transform: translateX(-50%) rotate(-3deg); } }
/* the controls */
#ts .ts-bar { position: absolute; left: 0; right: 0; bottom: 0; z-index: 3; display: flex; align-items: center; gap: 14px; padding: 14px max(20px, env(safe-area-inset-right)) max(18px, env(safe-area-inset-bottom)) max(20px, env(safe-area-inset-left)); }
#ts .ts-play { width: 50px; height: 50px; border-radius: 50%; background: ${C.yellow} !important; color: ${C.ink} !important; display: grid; place-items: center; flex: none; box-shadow: 0 4px 0 rgba(30,16,51,.5); transition: transform .15s; }
#ts .ts-play:hover { transform: scale(1.06) rotate(-6deg); }
#ts .ts-play:active { transform: scale(.94); }
#ts .ts-play svg { width: 20px; height: 20px; }
#ts .ts-track { position: relative; flex: 1; height: 28px; cursor: pointer; touch-action: none; }
#ts .ts-rail { position: absolute; left: 0; right: 0; top: 50%; height: 8px; margin-top: -4px; border-radius: 4px; background: rgba(30,16,51,.45); overflow: hidden; }
#ts .ts-fill { position: absolute; inset: 0; background: linear-gradient(90deg, ${C.yellow}, ${C.pink}, ${C.cyan}); transform-origin: 0 50%; transform: scaleX(0); }
#ts .ts-tick { position: absolute; top: 50%; width: 3px; height: 14px; margin: -7px 0 0 -1.5px; border-radius: 2px; background: rgba(255,255,255,.55); }
#ts .ts-knob { position: absolute; top: 50%; width: 18px; height: 18px; margin: -9px 0 0 -9px; border-radius: 50%; background: #fff; box-shadow: 0 0 0 4px ${C.pink}; }
#ts .ts-time { font-size: .875rem; font-weight: 800; font-variant-numeric: tabular-nums; min-width: 5.6em; text-align: right; text-shadow: 0 2px 0 rgba(30,16,51,.5); }
/* the cards at the start and the end */
#ts .ts-card { position: absolute; inset: 0; z-index: 4; display: grid; place-items: center; padding: 24px 16px; overflow-y: auto; background: radial-gradient(ellipse at 50% 35%, #B5179E, #2B0B6E 75%); text-align: center; transition: opacity .45s ease, transform .45s ease; }
#ts .ts-card[hidden] { display: none; }
#ts .ts-card.away { opacity: 0; transform: scale(1.08); pointer-events: none; }
#ts .ts-card h2 { font-size: clamp(2.5rem, min(10vw, 12vh), 6rem); line-height: .92; letter-spacing: -.05em; font-weight: 900; margin: 14px 0 12px; color: #fff; text-shadow: 0 .06em 0 ${C.pink}, 0 .12em 0 rgba(30,16,51,.6); transform: rotate(-2deg); }
#ts .ts-card h2 em { font-style: normal; color: ${C.yellow}; }
#ts .ts-card p { margin: 0 auto 26px; max-width: 30rem; color: rgba(255,255,255,.85); font-size: 1.125rem; line-height: 1.5; font-weight: 500; }
#ts .ts-card p b { color: ${C.yellow}; font-weight: 800; }
#ts .ts-go { display: inline-flex; align-items: center; gap: 12px; padding: 16px 30px; border-radius: 99px; background: ${C.yellow} !important; color: ${C.ink} !important; font-weight: 900 !important; font-size: 1.375rem !important; letter-spacing: -.02em; box-shadow: 0 6px 0 ${C.pink}, 0 14px 40px rgba(255,61,154,.45); transition: transform .15s, box-shadow .15s; }
#ts .ts-go:hover { transform: scale(1.05) rotate(-2deg); }
#ts .ts-go:active { transform: translateY(4px); box-shadow: 0 2px 0 ${C.pink}; }
#ts .ts-go svg { width: 20px; height: 20px; }
#ts .ts-keys { margin-top: 24px; font-size: .875rem; color: rgba(255,255,255,.7); }
#ts .ts-keys kbd { font: inherit; font-weight: 800; padding: 2px 8px; border-radius: 7px; background: rgba(30,16,51,.45); color: #fff; }
#ts .ts-row { display: flex; flex-wrap: wrap; gap: 10px; justify-content: center; }
#ts .ts-ghost { padding: 14px 22px; border-radius: 99px; background: rgba(30,16,51,.45) !important; font-weight: 800; }
#ts .ts-ghost:hover { background: rgba(30,16,51,.7) !important; }
#ts .ts-birdy { width: clamp(80px, 16vh, 130px); height: auto; animation: ts-hover 1.4s ease-in-out infinite; filter: drop-shadow(0 6px 0 rgba(30,16,51,.35)); }
#ts .ts-birdy .hb-wing { transform-box: fill-box; transform-origin: 92% 96%; animation: ts-flap .07s ease-in-out infinite alternate; }
#ts :focus-visible { outline: 3px solid ${C.yellow}; outline-offset: 3px; }
@keyframes ts-hover { 0%, 100% { transform: translateY(0) rotate(-4deg); } 50% { transform: translateY(-10px) rotate(-1deg); } }
@keyframes ts-flap { from { transform: rotate(-12deg); } to { transform: rotate(30deg) scale(.92, .55); } }
/* small screens, and short ones (a phone on its side) */
@media (max-width: 640px) {
  #ts .ts-name small { display: none; }
  #ts .ts-name svg { width: 36px; }
  #ts .ts-sec { font-size: .9375rem; top: max(70px, calc(env(safe-area-inset-top) + 58px)); }
  #ts .ts-hint { font-size: .8125rem; top: max(116px, calc(env(safe-area-inset-top) + 104px)); }
  #ts .ts-lyrics { bottom: calc(80px + env(safe-area-inset-bottom)); }
  #ts .ts-bar { gap: 10px; padding-top: 10px; }
  #ts .ts-time { min-width: 0; font-size: .8125rem; }
}
@media (max-height: 520px) {
  #ts .ts-top { padding-top: max(8px, env(safe-area-inset-top)); padding-bottom: 8px; }
  #ts .ts-name svg { width: 30px; }
  #ts .ts-name small { display: none; }
  #ts .ts-x { width: 38px; height: 38px; }
  #ts .ts-sec { top: 12px; font-size: .875rem; padding: 5px 12px 6px; }
  #ts .ts-hint { display: none; }
  #ts .ts-lyrics { bottom: calc(62px + env(safe-area-inset-bottom)); }
  #ts .ts-l { font-size: clamp(1.25rem, min(4.2vw, 8.5vh), 2.6rem); }
  #ts .ts-bar { padding-top: 6px; padding-bottom: max(10px, env(safe-area-inset-bottom)); }
  #ts .ts-play { width: 40px; height: 40px; }
  #ts .ts-card h2 { margin: 6px 0; }
  #ts .ts-card p { margin-bottom: 14px; font-size: 1rem; }
  #ts .ts-keys { display: none; }
}
@media (max-width: 360px) {
  #ts .ts-l { font-size: 1.375rem; }
  #ts .ts-sec { font-size: .8125rem; }
}
@media (prefers-reduced-motion: reduce) {
  #ts .ts-l { transition-duration: .01s; }
  #ts .ts-w.is-on { animation: none !important; }
  #ts .ts-birdy, #ts .ts-birdy .hb-wing { animation: none; }
}
`;

// the hummingbird (the site's mascot), as the page draws it; the stage draws it from two parts
// (its body, and its wing, which beats)
const BIRD_TAIL = `<path d="M40 68 L22 74 Q24 79 30 78 L26 84 Q31 87 35 83 L46 72 Z" fill="#E4A95A" stroke="#26262B" stroke-width="3"/>`;
const BIRD_BODY = `<path d="M97 40 C97 27 89 21 79 21 C69 21 63 27 61 35 C53 42 42 52 37 63 C35 70 39 74 47 72 C62 70 80 63 93 50 C96 47 97 44 97 40 Z" fill="#E4A95A" stroke="#26262B" stroke-width="3.4"/><path d="M48 69 C62 67 78 60 89 50" fill="none" stroke="#F4D6A2" stroke-width="5"/>`;
const BIRD_HEAD = `<path d="M96 36 L118 30" fill="none" stroke="#26262B" stroke-width="3.6"/><circle cx="84" cy="35" r="4.6" fill="#26262B"/><circle cx="85.7" cy="33.4" r="1.5" fill="#fff"/><ellipse cx="81" cy="45" rx="5" ry="2.8" fill="#F08F8A" opacity=".7"/>`;
const BIRD_WING = `<path d="M66 39 C54 35 40 26 33 12 C39 11 44 14 47 17 C46 11 47 7 50 5 C56 10 60 18 62 24 C64 20 67 18 70 18 C71 26 70 32 68 37 Z" fill="#FFF3DC" stroke="#26262B" stroke-width="3.1"/><path d="M47 18 C52 25 57 30 64 34 M62 25 C63 29 64 32 66 35" fill="none" stroke="#26262B" stroke-width="1.8" opacity=".35"/>`;
const svgOf = (inner, cls = "") => `<svg class="${cls}" viewBox="0 0 120 96" xmlns="http://www.w3.org/2000/svg"><g stroke-linecap="round" stroke-linejoin="round">${inner}</g></svg>`;
const BIRD_SVG = svgOf(`${BIRD_TAIL}${BIRD_BODY}<g class="hb-wing">${BIRD_WING}</g>${BIRD_HEAD}`, "ts-birdy");

const ICON = {
  play: `<svg viewBox="0 0 20 20" aria-hidden="true"><path d="M6 3.5v13l10.5-6.5z" fill="currentColor"/></svg>`,
  pause: `<svg viewBox="0 0 20 20" aria-hidden="true"><rect x="4.5" y="3.5" width="4" height="13" rx="1" fill="currentColor"/><rect x="11.5" y="3.5" width="4" height="13" rx="1" fill="currentColor"/></svg>`,
  close: `<svg viewBox="0 0 18 18" aria-hidden="true"><path d="M4 4l10 10M14 4L4 14" stroke="currentColor" stroke-width="2" stroke-linecap="round"/></svg>`,
};

// which words get a colour (and a way of moving) of their own
const VOICES = {
  "ts-tov": ["tov"],
  "ts-shout": ["oi", "agents", "pints", "up", "bust", "oi"],
  "ts-rust": ["rust", "rusts", "compiling"],
  "ts-fast": ["faster", "milliseconds", "fortytwo"],
  "ts-code": ["typescript", "json", "try", "curl", "pipe", "sh", "binary"],
  "ts-good": ["right", "fix", "fine", "done", "through", "straight", "ship"],
  "ts-bad": ["error", "never", "guesses", "poor", "missed"],
  "ts-small": ["little"],
  "ts-beer": ["pub", "tonight"],
  "ts-night": ["night", "grafter"],
  "ts-beep": ["tap", "bleep", "keys", "go"],
  "ts-big": ["ding", "five", "four", "three", "two", "one"],
};
const VOICE = new Map();
for (const [cls, ws] of Object.entries(VOICES)) for (const w of ws) if (!VOICE.has(w)) VOICE.set(w, cls);
function wordClass(text, before) {
  const w = text.toLowerCase().replace(/[^a-z0-9]/g, "");
  if (w === "any" || w === "null" || w === "equals") return "ts-code ts-no";
  // (an agent that never gets it right isn't right)
  if (w === "right" && before.includes("never")) return "ts-bad";
  const cls = VOICE.get(w) || "";
  // (shouts and the countdown are the words sung with a "!"; "agent's", "one little" aren't)
  if ((cls === "ts-shout" || cls === "ts-big") && !text.includes("!") && w !== "pints") return "";
  return cls;
}

let root, cv, ctx, audio, els = {};
let Wd = 0, Ht = 0, U = 1, dpr = 1;

function build() {
  const style = document.createElement("style");
  style.textContent = css;
  document.head.appendChild(style);

  root = document.createElement("div");
  root.id = "ts";
  root.hidden = true;
  root.setAttribute("role", "dialog");
  root.setAttribute("aria-modal", "true");
  root.setAttribute("aria-label", "Fast with Tov, a singalong");
  const lyricHtml = lines.map(l => `<p class="ts-l">${l.words.map((w, j) => {
    const shown = w.text.replace(/^\((.*)\)$/, "$1");
    const before = l.words.slice(0, j).map(x => x.text.toLowerCase());
    return `<span class="ts-w ${wordClass(shown, before)}">${shown}<i aria-hidden="true">${shown}</i></span>`;
  }).join(" ")}</p>`).join("");
  root.innerHTML = `
<div class="ts-sky" aria-hidden="true"></div><div class="ts-sky" aria-hidden="true"></div>
<canvas class="ts-cv" aria-hidden="true"></canvas>
<div class="ts-scan" aria-hidden="true"></div><div class="ts-flash" aria-hidden="true"></div>
<div class="ts-top">
  <div class="ts-name">${svgOf(`${BIRD_TAIL}${BIRD_BODY}${BIRD_WING}${BIRD_HEAD}`)}<span>Fast with Tov<small>a singalong</small></span></div>
  <button type="button" class="ts-x" aria-label="Close the singalong">${ICON.close}</button>
</div>
<div class="ts-sec" aria-hidden="true"></div>
<div class="ts-hint">Tap, click or mash any key on every <b>TOV!</b></div>
<div class="ts-lyrics">${lyricHtml}</div>
<div class="ts-bar">
  <button type="button" class="ts-play" aria-label="Play">${ICON.play}</button>
  <div class="ts-track" role="slider" tabindex="0" aria-label="Position in the song" aria-valuemin="0" aria-valuemax="0" aria-valuenow="0"><div class="ts-rail"><div class="ts-fill"></div></div><div class="ts-knob"></div></div>
  <span class="ts-time">0:00 / 0:00</span>
</div>
<div class="ts-card ts-start">
  <div>
    ${BIRD_SVG}
    <h2>Fast with <em>Tov</em></h2>
    <p>A singalong for coding agents and the people who buy them pints. Follow the words, and <b>tap, click or mash a key on every TOV!</b></p>
    <button type="button" class="ts-go">${ICON.play}Pints up!</button>
    <div class="ts-keys"><kbd>Space</kbd> pause · <kbd>←</kbd> <kbd>→</kbd> skip · <kbd>Esc</kbd> back to the page</div>
  </div>
</div>
<div class="ts-card ts-end" hidden>
  <div>
    ${BIRD_SVG}
    <h2>That's the <em>build</em> done!</h2>
    <p>Pints down, agents. Now the real thing:</p>
    <div class="ts-row">
      <button type="button" class="ts-go ts-again">${ICON.play}Again!</button>
      <button type="button" class="ts-ghost ts-copy">Copy the install line</button>
      <button type="button" class="ts-ghost ts-leave">Back to the page</button>
    </div>
  </div>
</div>`;
  document.body.appendChild(root);

  const q = s => root.querySelector(s);
  els = {
    sec: q(".ts-sec"), hint: q(".ts-hint"), skies: [...root.querySelectorAll(".ts-sky")], flash: q(".ts-flash"),
    play: q(".ts-play"), track: q(".ts-track"), fill: q(".ts-fill"), knob: q(".ts-knob"), time: q(".ts-time"),
    start: q(".ts-start"), lyrics: q(".ts-lyrics"), end: q(".ts-end"),
  };
  root.querySelectorAll(".ts-l").forEach((el, i) => {
    lines[i].el = el;
    el.querySelectorAll(".ts-w").forEach((w, j) => { lines[i].words[j].el = w; });
  });
  cv = q(".ts-cv");
  ctx = cv.getContext("2d");

  audio = new Audio();
  audio.preload = "auto";
  audio.src = SONG;
  audio.addEventListener("loadedmetadata", () => { drawTicks(); });
  audio.addEventListener("play", () => setPlaying(true));
  audio.addEventListener("pause", () => setPlaying(false));
  audio.addEventListener("ended", finish);
  audio.addEventListener("seeked", () => { anchor(audio.currentTime); resetLive(); });

  q(".ts-x").addEventListener("click", close);
  q(".ts-leave").addEventListener("click", close);
  q(".ts-go").addEventListener("click", e => { e.stopPropagation(); begin(); });
  q(".ts-again").addEventListener("click", e => { e.stopPropagation(); again(); });
  q(".ts-copy").addEventListener("click", async e => {
    e.stopPropagation();
    const b = e.currentTarget;
    try { await navigator.clipboard.writeText(INSTALL); } catch (err) { /* (no clipboard: the line is on the page) */ }
    b.textContent = "Copied: paste it in a terminal";
  });
  els.play.addEventListener("click", e => { e.stopPropagation(); toggle(); });
  bindTrack();
  cv.addEventListener("pointerdown", e => { if (e.button === 0) shout(e.clientX, e.clientY); });
  window.addEventListener("resize", resize);
  makeSprites();
}

// ---------------------------------------------------------------- the clock

// audio.currentTime moves in steps on some browsers: the stage runs on its own clock, anchored to
// the song's and nudged back whenever they drift apart.
let anchorT = 0, anchorAt = 0, lastA = -1;
function anchor(t) { anchorT = t; anchorAt = performance.now(); }
function songTime(now) {
  const a = audio.currentTime;
  if (audio.paused) { anchor(a); lastA = a; return a; }
  const t = anchorT + (now - anchorAt) / 1000 * audio.playbackRate;
  if (a !== lastA) {
    lastA = a;
    if (Math.abs(a - t) > 0.06) { anchor(a); return a; }
  }
  return t;
}

// ---------------------------------------------------------------- the stage: sprites

let birdBody = null, birdWing = null, glow = {};
function sprite(svg, w, h) {
  const c = document.createElement("canvas");
  c.width = w; c.height = h;
  const img = new Image();
  img.onload = () => c.getContext("2d").drawImage(img, 0, 0, w, h);
  img.src = "data:image/svg+xml;charset=utf-8," + encodeURIComponent(svg);
  return c;
}
function makeSprites() {
  birdBody = sprite(svgOf(`${BIRD_TAIL}${BIRD_BODY}${BIRD_HEAD}`), 360, 288);
  birdWing = sprite(svgOf(BIRD_WING), 360, 288);
  for (const [name, col] of Object.entries({ amber: C.amber, pink: C.pink, cyan: C.cyan, white: "#ffffff", violet: C.violet })) {
    const c = document.createElement("canvas");
    c.width = c.height = 128;
    const g = c.getContext("2d");
    const rg = g.createRadialGradient(64, 64, 0, 64, 64, 64);
    const [r, gg, b] = hex(col);
    rg.addColorStop(0, `rgba(${r},${gg},${b},1)`);
    rg.addColorStop(0.35, `rgba(${r},${gg},${b},.45)`);
    rg.addColorStop(1, `rgba(${r},${gg},${b},0)`);
    g.fillStyle = rg;
    g.fillRect(0, 0, 128, 128);
    glow[name] = c;
  }
}
function drawGlow(name, x, y, r, a) {
  if (a <= 0.003) return;
  ctx.globalAlpha = a;
  ctx.drawImage(glow[name], x - r, y - r, r * 2, r * 2);
  ctx.globalAlpha = 1;
}

// The canvas is kept to about 2.4 million pixels (a 4K screen gets a softer stage, not a slow one),
// and `quality` drops it further if frames run long.
const BUDGET = 2.4e6;
let quality = 1, sunGrad = null;
function resize() {
  if (!root || root.hidden) return;
  Wd = root.clientWidth; Ht = root.clientHeight;
  dpr = Math.max(0.5, Math.min(window.devicePixelRatio || 1, 2, Math.sqrt(BUDGET / (Wd * Ht))) * quality);
  cv.width = Math.round(Wd * dpr); cv.height = Math.round(Ht * dpr);
  // (a phone held upright gets a bigger stage: its width is all there is)
  U = Math.max(Math.min(Wd / (Wd < 700 ? 720 : 1100), Ht / 820), 0.42);
  sunGrad = null;
  pintCache.clear();
  drawTicks();
  for (const l of lines) l.el.style.setProperty("--h", `${l.el.offsetHeight}px`);
  lift();
}

// the line being sung sits just above the one coming next (whose height depends on its wrapping)
function lift() {
  const n = nextLine >= 0 ? lines[nextLine].el.offsetHeight * 0.5 : 0;
  els.lyrics.style.setProperty("--nh", `${n}px`);
}

// the middle of the stage, where things happen (above the lyrics)
const cx = () => Wd / 2;
// (sideways offsets shrink on a narrow screen, so things stay on it)
const narrow = () => Math.min(1, Wd / 1000);
// (a short screen, a phone on its side, puts it higher)
const cy = () => Ht * (Ht < 520 && Wd > Ht ? 0.33 : 0.38);

// ---------------------------------------------------------------- the stage: things in it

function bird(x, y, size, t, o = {}) {
  if (!birdBody) return;
  const s = size / 120;
  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(o.rot || 0);
  ctx.scale(o.flip ? -s : s, s);
  ctx.translate(-60, -48);
  ctx.drawImage(birdBody, 0, 0, 120, 96);
  ctx.translate(67, 38);
  const f = Math.sin(t * 75);
  ctx.rotate(0.15 + f * 0.45);
  ctx.scale(1, 0.72 + 0.28 * Math.cos(t * 75));
  ctx.translate(-67, -38);
  ctx.drawImage(birdWing, 0, 0, 120, 96);
  ctx.restore();
}

function speedLines(x, y, len, t, a, col = C.foam) {
  ctx.save();
  ctx.globalAlpha = a;
  ctx.strokeStyle = col;
  ctx.lineCap = "round";
  for (let i = 0; i < 5; i++) {
    const off = ((t * 3 + hash(i) ) % 1);
    ctx.lineWidth = (3 + i % 2 * 2) * U;
    const yy = y + (i - 2) * 11 * U;
    const x0 = x - len * (0.3 + off * 0.7) - 30 * U;
    ctx.beginPath();
    ctx.moveTo(x0, yy);
    ctx.lineTo(x0 + len * 0.35 * (1 - off), yy);
    ctx.stroke();
  }
  ctx.restore();
}

// a crab, the colour of rust; `mood` 0 (fine) to 1 (miserable)
function crab(x, y, size, t, mood = 0, walk = 1) {
  const s = size / 100;
  ctx.save();
  ctx.translate(x, y);
  ctx.scale(s, s);
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  ctx.strokeStyle = C.ink;
  // legs
  ctx.lineWidth = 6;
  for (let side = -1; side <= 1; side += 2) {
    for (let i = 0; i < 3; i++) {
      const ph = Math.sin(t * 9 * walk + i * 2 + (side > 0 ? 1 : 0)) * 6 * walk;
      ctx.beginPath();
      ctx.moveTo(side * (24 + i * 7), 8 + i * 3);
      ctx.lineTo(side * (46 + i * 8), 14 + i * 4 + ph);
      ctx.lineTo(side * (54 + i * 9), 32 + i * 2 + ph);
      ctx.stroke();
    }
  }
  // claws
  for (let side = -1; side <= 1; side += 2) {
    const snap = Math.max(0, Math.sin(t * 6 + side)) * 0.5 * (1 - mood);
    ctx.lineWidth = 7;
    ctx.beginPath();
    ctx.moveTo(side * 36, -6);
    ctx.quadraticCurveTo(side * 58, -18, side * 62, -34 + mood * 22);
    ctx.stroke();
    ctx.save();
    ctx.translate(side * 64, -42 + mood * 22);
    ctx.rotate(side * (0.3 + mood * 0.8));
    ctx.fillStyle = C.rust;
    ctx.lineWidth = 5;
    ctx.beginPath();
    ctx.ellipse(0, 0, 15, 12, 0, 0, TAU);
    ctx.fill(); ctx.stroke();
    ctx.fillStyle = "#2a120a";
    ctx.beginPath();
    ctx.moveTo(-2, -4);
    ctx.lineTo(side * 4, -16 - snap * 10);
    ctx.lineTo(side * 12, -8);
    ctx.closePath();
    ctx.fill();
    ctx.restore();
  }
  // body
  ctx.fillStyle = C.rust;
  ctx.lineWidth = 6;
  ctx.beginPath();
  ctx.ellipse(0, 0, 44, 30, 0, 0, TAU);
  ctx.fill(); ctx.stroke();
  ctx.fillStyle = "rgba(255,255,255,.18)";
  ctx.beginPath();
  ctx.ellipse(-12, -12, 18, 8, -0.3, 0, TAU);
  ctx.fill();
  // eyes on stalks
  for (let side = -1; side <= 1; side += 2) {
    ctx.lineWidth = 5;
    ctx.beginPath();
    ctx.moveTo(side * 11, -24);
    ctx.lineTo(side * 15, -44 + mood * 6);
    ctx.stroke();
    ctx.fillStyle = "#fff";
    ctx.beginPath();
    ctx.arc(side * 15, -48 + mood * 6, 10, 0, TAU);
    ctx.fill(); ctx.stroke();
    ctx.fillStyle = C.ink;
    ctx.beginPath();
    ctx.arc(side * 15 + 2, -46 + mood * 8, 4, 0, TAU);
    ctx.fill();
    if (mood > 0.4) {
      ctx.lineWidth = 4;
      ctx.beginPath();
      ctx.moveTo(side * 6, -60 + mood * 6);
      ctx.lineTo(side * 24, -54 + mood * 6 + side * 0);
      ctx.stroke();
    }
  }
  // mouth
  ctx.lineWidth = 4;
  ctx.beginPath();
  if (mood > 0.4) ctx.arc(0, 18, 10, Math.PI * 1.15, Math.PI * 1.85);
  else ctx.arc(0, 4, 10, Math.PI * 0.15, Math.PI * 0.85);
  ctx.stroke();
  // sweat
  if (mood > 0.2) {
    const d = (t * 1.3) % 1;
    ctx.globalAlpha = mood * (1 - d);
    ctx.fillStyle = C.cyan;
    ctx.beginPath();
    ctx.ellipse(38, -34 + d * 30, 5, 8, 0, 0, TAU);
    ctx.fill();
    ctx.globalAlpha = 1;
  }
  ctx.restore();
}

// a pint: `y` is the bottom of the glass
// a pint, drawn once per size and kept: `y` is the bottom of the glass
const pintCache = new Map();
function pint(x, y, h, rot = 0) {
  const k = Math.max(8, Math.round(h / 4) * 4);
  let c = pintCache.get(k);
  if (!c) { c = pintSprite(k); pintCache.set(k, c); }
  ctx.save();
  ctx.translate(x, y);
  ctx.rotate(rot);
  ctx.drawImage(c, -c.w / 2, -c.h + c.pad, c.w, c.h);
  ctx.restore();
}
function pintSprite(h) {
  const pad = h * 0.06, w = h * 0.86, hh = h * 1.2 + pad;
  const c = document.createElement("canvas");
  c.width = Math.ceil(w * dpr); c.height = Math.ceil(hh * dpr);
  c.w = w; c.h = hh; c.pad = pad;
  const g = c.getContext("2d");
  g.scale(dpr, dpr);
  g.translate(w / 2, hh - pad);
  const tw = h * 0.62, bw = h * 0.48;
  g.beginPath();
  g.moveTo(-bw / 2, 0); g.lineTo(-tw / 2, -h); g.lineTo(tw / 2, -h); g.lineTo(bw / 2, 0);
  g.closePath();
  g.save();
  g.clip();
  const lg = g.createLinearGradient(0, -h, 0, 0);
  lg.addColorStop(0, "#FFD34D");
  lg.addColorStop(1, "#F08A12");
  g.fillStyle = lg;
  g.fillRect(-tw, -h, tw * 2, h);
  g.fillStyle = "rgba(255,255,255,.55)";
  for (let i = 0; i < 6; i++) { g.beginPath(); g.arc((hash(i * 3) - 0.5) * bw * 0.8, -hash(i * 7) * h * 0.8, h * 0.02, 0, TAU); g.fill(); }
  g.fillStyle = "rgba(255,255,255,.25)";
  g.fillRect(-tw * 0.32, -h, tw * 0.1, h);
  g.restore();
  g.fillStyle = C.foam;
  g.beginPath();
  g.moveTo(-tw / 2 - h * 0.02, -h * 0.84);
  g.lineTo(-tw / 2 - h * 0.02, -h);
  for (let i = 0; i <= 4; i++) g.arc(-tw / 2 + (tw * i) / 4, -h - h * 0.02 + Math.sin(i * 1.7) * h * 0.015, h * 0.09, Math.PI, 0);
  g.lineTo(tw / 2 + h * 0.02, -h * 0.84);
  g.closePath();
  g.fill();
  g.strokeStyle = C.ink;
  g.lineWidth = Math.max(1.5, h * 0.035);
  g.lineJoin = "round";
  g.beginPath();
  g.moveTo(-bw / 2, 0); g.lineTo(-tw / 2, -h * 0.84); g.moveTo(bw / 2, 0); g.lineTo(tw / 2, -h * 0.84); g.moveTo(-bw / 2, 0); g.lineTo(bw / 2, 0);
  g.stroke();
  return c;
}

// a coding agent: a little robot. face: "happy", "dots", "?", "x", "type"
function bot(x, y, size, t, face = "dots") {
  const s = size / 100;
  ctx.save();
  ctx.translate(x, y);
  ctx.scale(s, s);
  ctx.lineJoin = "round";
  ctx.lineCap = "round";
  ctx.strokeStyle = C.ink;
  ctx.lineWidth = 5;
  // antenna
  ctx.beginPath();
  ctx.moveTo(0, -46);
  ctx.lineTo(0, -66);
  ctx.stroke();
  const blink = (Math.sin(t * 6) + 1) / 2;
  ctx.fillStyle = blink > 0.5 ? C.amber : "#8a5a20";
  ctx.beginPath();
  ctx.arc(0, -70, 8, 0, TAU);
  ctx.fill(); ctx.stroke();
  // head
  ctx.fillStyle = "#EDE7F6";
  roundRect(-50, -46, 100, 84, 22);
  ctx.fill(); ctx.stroke();
  ctx.fillStyle = "#1b1830";
  roundRect(-38, -34, 76, 56, 14);
  ctx.fill();
  // ears
  ctx.fillStyle = C.amber;
  roundRect(-60, -16, 12, 30, 5); ctx.fill(); ctx.stroke();
  roundRect(48, -16, 12, 30, 5); ctx.fill(); ctx.stroke();
  // face
  ctx.strokeStyle = C.cyan;
  ctx.fillStyle = C.cyan;
  ctx.lineWidth = 5;
  if (face === "happy") {
    for (const ex of [-16, 16]) { ctx.beginPath(); ctx.arc(ex, -2, 9, Math.PI * 1.1, Math.PI * 1.9); ctx.stroke(); }
    ctx.beginPath(); ctx.arc(0, 4, 12, Math.PI * 0.2, Math.PI * 0.8); ctx.stroke();
  } else if (face === "?") {
    ctx.font = `800 30px ${FONT}`;
    ctx.textAlign = "center"; ctx.textBaseline = "middle";
    ctx.fillText("?", -16, -6); ctx.fillText("?", 16, -6);
  } else if (face === "x") {
    ctx.strokeStyle = C.red;
    for (const ex of [-16, 16]) { ctx.beginPath(); ctx.moveTo(ex - 7, -13); ctx.lineTo(ex + 7, 1); ctx.moveTo(ex + 7, -13); ctx.lineTo(ex - 7, 1); ctx.stroke(); }
    ctx.beginPath(); ctx.moveTo(-10, 12); ctx.lineTo(10, 12); ctx.stroke();
  } else {
    const look = face === "type" ? Math.sin(t * 9) * 3 : 0;
    const sh = (t % 3) < 0.12 ? 0.15 : 1;
    for (const ex of [-16, 16]) { ctx.beginPath(); ctx.ellipse(ex + look, -6, 6, 8 * sh, 0, 0, TAU); ctx.fill(); }
    ctx.beginPath(); ctx.arc(0, 4, 8, Math.PI * 0.2, Math.PI * 0.8); ctx.stroke();
  }
  ctx.restore();
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + h, r);
  ctx.arcTo(x + w, y + h, x, y + h, r);
  ctx.arcTo(x, y + h, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}

// a window with a title bar (an editor or a terminal); returns its content box
function panel(x, y, w, h, title, a = 1) {
  ctx.save();
  ctx.globalAlpha = a;
  ctx.fillStyle = "rgba(0,0,0,.35)";
  roundRect(x + 8 * U, y + 12 * U, w, h, 16 * U);
  ctx.fill();
  ctx.fillStyle = "#17141d";
  roundRect(x, y, w, h, 16 * U);
  ctx.fill();
  ctx.strokeStyle = "rgba(255,243,220,.16)";
  ctx.lineWidth = 1.5;
  ctx.stroke();
  ctx.fillStyle = "rgba(255,255,255,.05)";
  ctx.fillRect(x + 1, y + 36 * U, w - 2, 1.5);
  [C.red, C.amber, C.green].forEach((col, i) => {
    ctx.fillStyle = col;
    ctx.beginPath();
    ctx.arc(x + (20 + i * 18) * U, y + 18 * U, 5.5 * U, 0, TAU);
    ctx.fill();
  });
  ctx.fillStyle = "rgba(255,243,220,.5)";
  ctx.font = `600 ${13 * U}px ${MONO}`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(title, x + w / 2, y + 18 * U);
  ctx.restore();
  return { x: x + 22 * U, y: y + 58 * U, w: w - 44 * U };
}

function mono(text, x, y, size, col, a = 1, align = "left") {
  ctx.globalAlpha = a;
  ctx.fillStyle = col;
  ctx.font = `600 ${size}px ${MONO}`;
  ctx.textAlign = align;
  ctx.textBaseline = "middle";
  ctx.fillText(text, x, y);
  ctx.globalAlpha = 1;
}

function bigText(text, x, y, size, col, o = {}) {
  ctx.save();
  ctx.translate(x, y);
  if (o.rot) ctx.rotate(o.rot);
  if (o.scale) ctx.scale(o.scale, o.scale);
  ctx.globalAlpha = o.a ?? 1;
  ctx.font = `${o.weight || 900} ${size}px ${o.font || FONT}`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.lineJoin = "round";
  // (an extruded side in a second colour, then the ink outline, then the face)
  const depth = o.extrude ? 4 : 1, step = size * (o.extrude ? 0.022 : 0.06);
  ctx.fillStyle = o.extrude || "rgba(30,16,51,.4)";
  for (let i = depth; i >= 1; i--) ctx.fillText(text, step * i * 0.6, step * i);
  ctx.lineWidth = size * (o.stroke ?? 0.1);
  ctx.strokeStyle = o.strokeCol || C.ink;
  ctx.strokeText(text, 0, 0);
  ctx.fillStyle = col;
  ctx.fillText(text, 0, 0);
  ctx.restore();
}

// a comic-book burst, behind a word that's shouted
function starburst(x, y, r, col, rot, a = 1) {
  ctx.save();
  ctx.globalAlpha = a;
  ctx.translate(x, y);
  ctx.rotate(rot);
  ctx.fillStyle = col;
  ctx.strokeStyle = C.ink;
  ctx.lineWidth = Math.max(2, r * 0.035);
  ctx.lineJoin = "round";
  ctx.beginPath();
  const n = 14;
  for (let i = 0; i < n * 2; i++) {
    const rr = i % 2 ? r * (0.62 + hash(i) * 0.12) : r * (0.92 + hash(i * 3) * 0.16);
    const ang = (i / (n * 2)) * TAU;
    ctx.lineTo(Math.cos(ang) * rr, Math.sin(ang) * rr * 0.78);
  }
  ctx.closePath();
  ctx.fill();
  ctx.stroke();
  ctx.restore();
}

function squiggle(x0, x1, y, amp, col, a, width) {
  ctx.save();
  ctx.globalAlpha = a;
  ctx.strokeStyle = col;
  ctx.lineWidth = width;
  ctx.lineCap = "round";
  ctx.beginPath();
  for (let x = x0; x <= x1; x += 2) ctx.lineTo(x, y + Math.sin((x - x0) / (amp * 1.6)) * amp);
  ctx.stroke();
  ctx.restore();
}

function check(x, y, s, col, k = 1) {
  ctx.save();
  ctx.strokeStyle = col;
  ctx.lineWidth = s * 0.18;
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  ctx.beginPath();
  const p1 = [x - s * 0.4, y], p2 = [x - s * 0.1, y + s * 0.3], p3 = [x + s * 0.45, y - s * 0.35];
  ctx.moveTo(...p1);
  const k1 = clamp(k * 2, 0, 1), k2 = clamp(k * 2 - 1, 0, 1);
  ctx.lineTo(lerp(p1[0], p2[0], k1), lerp(p1[1], p2[1], k1));
  if (k2 > 0) ctx.lineTo(lerp(p2[0], p3[0], k2), lerp(p2[1], p3[1], k2));
  ctx.stroke();
  ctx.restore();
}

// ---------------------------------------------------------------- particles

const P = [];
let MAXP = calm ? 120 : 500;
function add(p) {
  if (P.length >= MAXP) return;
  p.age = 0;
  P.push(p);
}
const CONF = [C.yellow, C.pink, C.cyan, C.lime, "#fff", C.violet, C.orange];
function confetti(n, x, y, spread = 1) {
  for (let i = 0; i < n; i++) {
    const a = Math.random() * TAU, v = (200 + Math.random() * 600) * U * spread;
    add({ k: "conf", x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v - 300 * U, g: 900 * U, drag: 1.6, life: 1.8 + Math.random() * 1.4, s: (6 + Math.random() * 8) * U, rot: Math.random() * TAU, vr: (Math.random() - 0.5) * 18, col: CONF[i % CONF.length] });
  }
}
function rainConfetti(n) {
  for (let i = 0; i < n; i++) {
    add({ k: "conf", x: Math.random() * Wd, y: -20, vx: (Math.random() - 0.5) * 80 * U, vy: (120 + Math.random() * 160) * U, g: 40 * U, drag: 0.2, life: 6, s: (6 + Math.random() * 7) * U, rot: Math.random() * TAU, vr: (Math.random() - 0.5) * 10, col: CONF[i % CONF.length] });
  }
}
function sparks(n, x, y, col, speed = 1) {
  for (let i = 0; i < n; i++) {
    const a = Math.random() * TAU, v = (300 + Math.random() * 700) * U * speed;
    add({ k: "spark", x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v, g: 300 * U, drag: 3, life: 0.5 + Math.random() * 0.4, s: (2 + Math.random() * 3) * U, col });
  }
}
function foam(n, x, y) {
  for (let i = 0; i < n; i++) {
    const a = -Math.PI / 2 + (Math.random() - 0.5) * 2.4, v = (150 + Math.random() * 450) * U;
    add({ k: "bub", x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v, g: 700 * U, drag: 1.2, life: 0.9 + Math.random() * 0.6, s: (4 + Math.random() * 10) * U, col: Math.random() < 0.3 ? C.amber : C.foam });
  }
}
function ring(x, y, col, r = 160, life = 0.6, w = 6) {
  add({ k: "ring", x, y, vx: 0, vy: 0, g: 0, drag: 0, life, s: r * U, w: w * U, col });
}
function floatText(text, x, y, col, size = 34, life = 1) {
  add({ k: "txt", x, y, vx: 0, vy: -90 * U, g: 0, drag: 1, life, s: size * U, col, text, rot: (Math.random() - 0.5) * 0.3 });
}
function pixels(n, x, y) {
  for (let i = 0; i < n; i++) {
    const a = Math.random() * TAU, v = (200 + Math.random() * 500) * U;
    add({ k: "pix", x, y, vx: Math.cos(a) * v, vy: Math.sin(a) * v, g: 500 * U, drag: 1.5, life: 0.8 + Math.random() * 0.5, s: (6 + Math.floor(Math.random() * 3) * 4) * U, col: [C.cyan, C.pink, C.green, C.amber][i % 4] });
  }
}

function stepParticles(dt) {
  for (let i = P.length - 1; i >= 0; i--) {
    const p = P[i];
    p.age += dt;
    if (p.age >= p.life) { P[i] = P[P.length - 1]; P.pop(); continue; }
    const d = Math.exp(-p.drag * dt);
    p.vx *= d; p.vy = p.vy * d + p.g * dt;
    p.x += p.vx * dt; p.y += p.vy * dt;
    if (p.vr) p.rot += p.vr * dt;
  }
}

function drawParticles() {
  for (const p of P) {
    const k = p.age / p.life, a = 1 - k * k;
    ctx.globalAlpha = a;
    if (p.k === "conf") {
      ctx.save();
      ctx.translate(p.x, p.y);
      ctx.rotate(p.rot);
      ctx.scale(1, Math.cos(p.rot * 1.7));
      ctx.fillStyle = p.col;
      ctx.fillRect(-p.s / 2, -p.s / 4, p.s, p.s / 2);
      ctx.restore();
    } else if (p.k === "spark") {
      ctx.strokeStyle = p.col;
      ctx.lineWidth = p.s;
      ctx.lineCap = "round";
      ctx.beginPath();
      ctx.moveTo(p.x, p.y);
      ctx.lineTo(p.x - p.vx * 0.03, p.y - p.vy * 0.03);
      ctx.stroke();
    } else if (p.k === "bub") {
      ctx.fillStyle = p.col;
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.s * (1 - k * 0.5), 0, TAU);
      ctx.fill();
    } else if (p.k === "ring") {
      ctx.strokeStyle = p.col;
      ctx.lineWidth = p.w * (1 - k);
      ctx.beginPath();
      ctx.arc(p.x, p.y, p.s * out3(k), 0, TAU);
      ctx.stroke();
    } else if (p.k === "txt") {
      const sc = k < 0.15 ? back(k / 0.15) : 1;
      bigText(p.text, p.x, p.y, p.s, p.col, { a, rot: p.rot, scale: sc, stroke: 0.14 });
    } else if (p.k === "pix") {
      ctx.fillStyle = p.col;
      ctx.fillRect(Math.round(p.x / 4) * 4, Math.round(p.y / 4) * 4, p.s, p.s);
    }
  }
  ctx.globalAlpha = 1;
}

// ---------------------------------------------------------------- the stage: what happens when

// scenes draw while their time is on (fading in and out); triggers fire once as the song passes
// them (so they make particles and shakes, which a scene can't undo).
const scenes = [], triggers = [], targets = [];
function scene(s, e, draw, fade = 0.3) { scenes.push({ s, e, draw, fade }); }
function on(t, fn) { triggers.push({ t, fn }); }
function target(t, label) { targets.push({ t, label, popped: false }); }

let shakeAmt = 0, flashAmt = 0, zoomAmt = 0;
function shake(n) { if (!calm) shakeAmt = Math.max(shakeAmt, n); }
function flash(a, col = "#fff") { if (!calm) { flashAmt = Math.max(flashAmt, a); els.flash.style.background = col; } }
function punch(n) { if (!calm) zoomAmt = Math.max(zoomAmt, n); }

// the colours a stamp comes in, one after another: face, extruded side, burst behind
const LOUD = [[C.yellow, C.pink, C.cyan], [C.cyan, C.violet, C.yellow], [C.lime, C.blue, C.pink], [C.pink, C.yellow, C.lime], [C.orange, C.cyan, C.violet], ["#fff", C.pink, C.lime]];
let stamps = 0;

// a word slammed onto the stage
function stamp(t, text, o = {}) {
  const life = o.life ?? 0.8;
  const [face, side, back_] = LOUD[stamps++ % LOUD.length];
  const col = o.col || face, ext = o.extrude || side, burst = o.burst ?? back_;
  const spin = (hash(stamps) - 0.5) * 0.6;
  const at = () => [cx() + (o.dx ?? 0) * U * narrow(), (o.y ? Ht * o.y : cy() - 40 * U) + (o.dy ?? 0) * U];
  scene(t, t + life, now => {
    const dt = now - t;
    const k = clamp(dt / 0.11, 0, 1);
    let sc = lerp(3, 1, out3(k));
    if (k >= 1) sc = 1 + Math.sin((dt - 0.11) * 30) * 0.07 * Math.exp(-(dt - 0.11) * 6);
    // (it leaves by shrinking away: fading would show through its extruded layers)
    const gone = clamp((dt - (life - 0.16)) / 0.16, 0, 1);
    sc *= 1 - gone * gone;
    const a = Math.min(k * 3, 1);
    if (sc <= 0.01) return;
    const [x, y] = at();
    const size = (o.size ?? 200) * U;
    if (burst) starburst(x, y, size * 0.95 * back(dt / 0.25), burst, spin + dt * 0.8, a);
    bigText(text, x, y, size, col, { a, rot: o.rot ?? 0, scale: sc, stroke: 0.08, extrude: ext });
  }, 0.001);
  on(t, () => {
    const [x, y] = at();
    shake(o.shake ?? 14);
    punch(0.05);
    flash(0.14, burst || col);
    ring(x, y, col, 280, 0.5, 12);
    sparks(o.sparks ?? 22, x, y, col);
  });
}

function fadeIn(now, s, d = 0.35) { return clamp((now - s) / d, 0, 1); }

// ---- the intro: Oi! Agents! Pints up!
function intro(li) {
  const oi = W(li, 0), agents = W(li, 1), pints = W(li, 2), up = W(li, 3);
  stamp(oi.s, "OI!", { size: 260, rot: -0.12, shake: 22, sparks: 30 });
  target(oi.s, "OI!");
  scene(agents.s - 0.1, up.e + 4.2, now => {
    const n = Wd < 700 ? 3 : 5;
    for (let i = 0; i < n; i++) {
      const pop = back((now - agents.s - i * 0.07) / 0.45);
      const x = Wd * (i + 0.5) / n, y = cy() + 70 * U + (1 - pop) * 400 * U;
      const bob = Math.sin(now * 8 + i) * 6 * U;
      bot(x, y + bob, 120 * U, now + i, now > up.s ? "happy" : "dots");
      // the pints go up
      const raise = out3((now - pints.s - i * 0.04) / 0.35);
      if (raise > 0) {
        const clink = now > up.s ? Math.sin(clamp((now - up.s) / 0.25, 0, 1) * Math.PI) : 0;
        const px = x + 80 * U - clink * 26 * U, py = y - 30 * U - raise * 130 * U;
        pint(px, py, 80 * U, -0.15 * clink + Math.sin(now * 5 + i) * 0.05);
      }
    }
  }, 0.4);
  on(up.s + 0.12, () => {
    const n = Wd < 700 ? 3 : 5;
    for (let i = 0; i < n; i++) foam(14, Wd * (i + 0.5) / n + 54 * U, cy() - 170 * U);
    shake(8);
  });
}

// ---- a chorus (4 lines from `b`): Tov! Tov! Faster than Rust! ...
function chorus(b, last) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  // Tov! Tov!
  stamp(W(b, 2).s, "TOV!", { rot: -0.1, dx: -150 });
  stamp(W(b, 3).s, "TOV!", { rot: 0.1, dx: 150 });
  target(W(b, 2).s, "TOV"); target(W(b, 3).s, "TOV");
  // faster than Rust: the hummingbird laps the crab
  race(W(b, 4).s, l1.s + 0.3);
  // written like TypeScript: the code rains
  scene(l1.s, W(b + 1, 6).s, now => codeRain(now, l1.s, fadeIn(now, l1.s, 0.3)), 0.3);
  stamp(W(b + 1, 4).s, "TOV", { size: 150, dy: -40, life: 0.6 });
  target(W(b + 1, 4).s, "TOV");
  // or bust!
  const bust = W(b + 1, 6);
  stamp(bust.s, "BUST!", { size: 170, col: C.pink, glow: "pink", rot: 0.08, shake: 20, sparks: 10 });
  on(bust.s, () => confetti(50, cx(), cy() - 40 * U, 1.2));
  // your agent writes it and Tov puts it right
  agentFix(l2);
  target(W(b + 2, 5).s, "TOV");
  // ship one little binary, down the pub tonight
  ship(l3);
  if (last) {
    scene(l0.s, l3.e + 1, now => { if (Math.random() < 0.5) rainConfetti(1); }, 0.1);
  }
}

function race(s, e) {
  scene(s - 0.1, e, now => {
    const k = clamp((now - s) / (e - s), 0, 1);
    const y = Ht * 0.48;
    // track
    ctx.save();
    ctx.globalAlpha = fadeIn(now, s - 0.1, 0.25) * 0.35;
    ctx.strokeStyle = C.foam;
    ctx.setLineDash([22 * U, 18 * U]);
    ctx.lineDashOffset = -now * 600 * U;
    ctx.lineWidth = 3 * U;
    ctx.beginPath(); ctx.moveTo(0, y + 50 * U); ctx.lineTo(Wd, y + 50 * U); ctx.stroke();
    ctx.restore();
    // the crab plods
    const cxp = lerp(Wd * 0.06, Wd * 0.36, k);
    crab(cxp, y + 10 * U, 90 * U, now, 0.15 + k * 0.6, 1.4);
    mono("rustc", cxp, y + 72 * U, 13 * U, "rgba(255,243,220,.6)", 1, "center");
    // the bird flies
    const bk = out3(k * 1.5);
    const bx = lerp(-120 * U, Wd + 160 * U, bk), by = y - 40 * U + Math.sin(now * 14) * 8 * U;
    speedLines(bx - 40 * U, by, 260 * U, now, 0.8);
    bird(bx, by, 150 * U, now, { rot: -0.08 });
  }, 0.25);
  on(s, () => shake(6));
}

const TOKENS = ["const", "=>", "{ }", ": string", "async", "await", "interface", "type", "import", "export", "fetch()", "try", "Bun.serve", "?.", "[]", "<T>", "return", "let"];
function codeRain(now, s, a) {
  const n = Wd < 700 ? 14 : 26;
  ctx.save();
  ctx.globalAlpha = a;
  for (let i = 0; i < n; i++) {
    const speed = 260 + hash(i) * 340;
    const x = (hash(i * 7.1) * 1.1 - 0.05) * Wd;
    const y = -60 * U + ((now - s) * speed * U + hash(i * 3.3) * Ht * 0.6) % (Ht * 0.75);
    const tok = TOKENS[i % TOKENS.length];
    const size = (18 + hash(i * 1.7) * 22) * U;
    ctx.font = `700 ${size}px ${MONO}`;
    ctx.textAlign = "center";
    ctx.fillStyle = i % 3 === 0 ? C.cyan : i % 3 === 1 ? "#9DB2F5" : C.foam;
    ctx.globalAlpha = a * (0.45 + hash(i * 9) * 0.55);
    ctx.fillText(tok, x, y);
  }
  ctx.restore();
}

function agentFix(l) {
  const writes = l.words[2], tov = l.words[5], right = l.words[8];
  scene(l.s - 0.15, Math.min(l.e + 0.5, until(l.i) - 0.1), now => {
    const a = fadeIn(now, l.s - 0.15, 0.3);
    const w = Math.min(560 * U, Wd - 32), h = 190 * U;
    const x = cx() - w / 2, y = cy() - h / 2 + 20 * U;
    const box = panel(x, y + (1 - out3(a)) * 40 * U, w, h, "hello.tov", a);
    const fs = Math.max(13, 22 * U);
    // the agent types the line
    const line = "const url = new URL(req.url)";
    const typed = Math.floor(clamp((now - l.s) / Math.max(0.3, writes.e - l.s), 0, 1) * line.length);
    const fixed = now >= right.s;
    const prefix = "const url = ";
    ctx.font = `600 ${fs}px ${MONO}`;
    const cw = ctx.measureText("m").width;
    const ly = box.y + 22 * U;
    mono("4", box.x, ly, fs, "rgba(255,243,220,.3)", a);
    const tx = box.x + cw * 2;
    if (!fixed) {
      mono(line.slice(0, typed), tx, ly, fs, C.foam, a);
      if (typed < line.length && Math.floor(now * 4) % 2 === 0) mono("▌", tx + cw * typed, ly, fs, C.amber, a);
      if (typed >= prefix.length + 6) {
        const wob = now > tov.s ? 1 + Math.sin(now * 30) * 0.2 : 1;
        squiggle(tx + cw * prefix.length, tx + cw * line.length, ly + fs * 0.75, 3 * U * wob, C.red, a, 2.5 * U);
        mono("T0831: `new URL` can throw", box.x, ly + fs * 2.2, fs * 0.72, C.red, a * clamp((now - writes.e) / 0.2, 0, 1));
      }
    } else {
      const k = clamp((now - right.s) / 0.25, 0, 1);
      mono(prefix, tx, ly, fs, C.foam, a);
      const tryW = cw * 4 * back(k);
      ctx.save();
      ctx.globalAlpha = a;
      ctx.fillStyle = "rgba(228,169,90,.25)";
      roundRect(tx + cw * prefix.length - 3, ly - fs * 0.7, tryW + 2, fs * 1.4, 5);
      ctx.fill();
      ctx.restore();
      mono("try", tx + cw * prefix.length, ly, fs, C.amber, a * k);
      mono("new URL(req.url)", tx + cw * prefix.length + tryW, ly, fs, C.foam, a);
      mono("✓ fixed, build goes through", box.x, ly + fs * 2.2, fs * 0.72, C.green, a * k);
    }
    // the bird swoops in to put it right
    if (now > tov.s - 0.3) {
      const k = out3((now - tov.s + 0.3) / 0.5);
      const bx = lerp(Wd + 100 * U, x + w - 60 * U, k), by = lerp(y - 120 * U, y - 30 * U, k) + Math.sin(now * 10) * 5 * U;
      bird(bx, by, 110 * U, now, { flip: true });
    }
  }, 0.3);
  on(right.s, () => { sparks(20, cx(), cy(), C.green); ring(cx(), cy(), C.green, 240, 0.5, 8); });
}

function ship(l) {
  const binary = l.words[3], pub = l.words[6], tonight = l.words[7];
  const end = Math.min(l.e + 0.9, until(l.i) + 0.1);
  scene(l.s - 0.1, end, now => {
    const a = fadeIn(now, l.s - 0.1, 0.25) * clamp((end - now) / 0.3, 0, 1);
    const sea = Ht * 0.56;
    // waves
    ctx.save();
    ctx.globalAlpha = a;
    for (let r = 0; r < 3; r++) {
      ctx.fillStyle = ["#1d5a8a", "#2a73a8", "#3d8fc7"][r];
      ctx.beginPath();
      ctx.moveTo(0, Ht);
      for (let x = 0; x <= Wd; x += 8) ctx.lineTo(x, sea + r * 22 * U + Math.sin(x / (60 * U) + now * (2 + r) + r) * 8 * U);
      ctx.lineTo(Wd, Ht);
      ctx.closePath();
      ctx.globalAlpha = a * (0.55 + r * 0.15);
      ctx.fill();
    }
    ctx.restore();
    // the ship, carrying one little binary
    const k = clamp((now - l.s) / (pub.s - l.s), 0, 1);
    const sx = lerp(-160 * U, cx() - 120 * U, out3(k)) + Math.max(0, now - pub.s) * 120 * U;
    const sy = sea - 6 * U + Math.sin(now * 3) * 6 * U, rock = Math.sin(now * 3.2) * 0.06;
    ctx.save();
    ctx.globalAlpha = a;
    ctx.translate(sx, sy);
    ctx.rotate(rock);
    ctx.scale(U, U);
    ctx.lineJoin = "round";
    ctx.strokeStyle = C.ink;
    ctx.lineWidth = 5;
    ctx.fillStyle = "#6b3d1c";
    ctx.beginPath(); ctx.moveTo(-110, -20); ctx.lineTo(110, -20); ctx.lineTo(80, 24); ctx.lineTo(-80, 24); ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(0, -20); ctx.lineTo(0, -170); ctx.stroke();
    ctx.fillStyle = C.foam;
    ctx.beginPath(); ctx.moveTo(6, -165); ctx.quadraticCurveTo(70, -110, 90, -40); ctx.lineTo(6, -40); ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.fillStyle = C.amber;
    ctx.beginPath(); ctx.moveTo(0, -170); ctx.lineTo(40, -158); ctx.lineTo(0, -146); ctx.closePath(); ctx.fill(); ctx.stroke();
    // the binary: a crate
    const pop = back((now - binary.s) / 0.35);
    if (pop > 0) {
      ctx.save();
      ctx.translate(-55, -20);
      ctx.scale(pop, pop);
      ctx.fillStyle = "#2b2733";
      roundRect(-42, -62, 84, 62, 8); ctx.fill(); ctx.stroke();
      ctx.fillStyle = C.green;
      ctx.font = `700 15px ${MONO}`;
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      ctx.fillText("0101", 0, -42);
      ctx.fillStyle = C.foam;
      ctx.fillText("152K", 0, -20);
      ctx.restore();
    }
    ctx.restore();
    // the pub
    if (now > pub.s - 0.2) pubSign(cx() + 170 * U, Ht * 0.16, now, pub.s - 0.2, a);
    if (now > tonight.s) {
      const k2 = out3((now - tonight.s) / 0.3);
      for (const side of [-1, 1]) {
        const clink = Math.sin(clamp((now - tonight.s - 0.15) / 0.3, 0, 1) * Math.PI);
        pint(cx() + 170 * U + side * (90 - clink * 34) * U, Ht * 0.16 + 330 * U - k2 * 40 * U, 90 * U, -side * 0.2 * clink);
      }
    }
  }, 0.25);
  on(tonight.s + 0.2, () => foam(30, cx() + 170 * U, Ht * 0.16 + 200 * U));
}

function pubSign(x, y, now, s, a = 1) {
  const k = clamp((now - s) / 0.5, 0, 1);
  const drop = (1 - back(k)) * -260 * U;
  const swing = Math.sin((now - s) * 5) * 0.25 * Math.exp(-(now - s) * 1.2) + Math.sin(now * 1.3) * 0.03;
  ctx.save();
  ctx.globalAlpha = a;
  ctx.translate(x, y + drop);
  ctx.strokeStyle = C.ink;
  ctx.lineWidth = 6 * U;
  ctx.beginPath(); ctx.moveTo(-120 * U, -10 * U); ctx.lineTo(120 * U, -10 * U); ctx.stroke();
  ctx.rotate(swing);
  ctx.strokeStyle = "#c9a35a";
  ctx.lineWidth = 3 * U;
  ctx.beginPath(); ctx.moveTo(-80 * U, -10 * U); ctx.lineTo(-80 * U, 30 * U); ctx.moveTo(80 * U, -10 * U); ctx.lineTo(80 * U, 30 * U); ctx.stroke();
  ctx.fillStyle = "#4a2410";
  roundRect(-130 * U, 30 * U, 260 * U, 130 * U, 14 * U); ctx.fill();
  ctx.strokeStyle = "#E4C07A";
  ctx.lineWidth = 4 * U;
  roundRect(-120 * U, 40 * U, 240 * U, 110 * U, 10 * U); ctx.stroke();
  ctx.fillStyle = "#F2D79B";
  ctx.font = `800 ${15 * U}px ${FONT}`;
  ctx.textAlign = "center"; ctx.textBaseline = "middle";
  ctx.fillText("T H E", 0, 62 * U);
  ctx.font = `900 ${34 * U}px ${FONT}`;
  ctx.fillText("TOV ARMS", 0, 96 * U);
  ctx.font = `600 ${12 * U}px ${FONT}`;
  ctx.fillText("est. v0.0.1 · free house", 0, 130 * U);
  ctx.restore();
}

// ---- verse one: the agent codes all night, guesses, and Tov tells it straight
function verse1(b) {
  const l0 = L(b), l2 = L(b + 2), l3 = L(b + 3);
  const night = W(b, 7), guesses = W(b + 1, 2), never = W(b + 1, 7), right = W(b + 1, 10);
  const tov = W(b + 2, 1), straight = W(b + 2, 4);
  const err = W(b + 3, 2), fix = W(b + 3, 5), through = W(b + 3, 10);
  target(tov.s, "TOV");
  // the moon comes up
  scene(l0.s - 1, l3.e + 0.5, now => {
    const k = out3((now - l0.s + 1) / 2);
    const mx = Wd * 0.82, my = lerp(Ht * 0.5, Ht * 0.17, k);
    drawGlow("white", mx, my, 160 * U, 0.25);
    ctx.fillStyle = "#F6EFD8";
    ctx.beginPath(); ctx.arc(mx, my, 46 * U, 0, TAU); ctx.fill();
    ctx.fillStyle = "rgba(0,0,0,.08)";
    for (const [dx, dy, r] of [[-14, -10, 9], [12, 8, 12], [-6, 18, 6]]) { ctx.beginPath(); ctx.arc(mx + dx * U, my + dy * U, r * U, 0, TAU); ctx.fill(); }
    if (now > night.s) {
      const n = (now - night.s);
      for (let i = 0; i < 3; i++) bigText("z", mx - 70 * U - i * 26 * U, my + 20 * U - ((n * 40 + i * 30) % 90) * U, (20 + i * 6) * U, C.foam, { a: 0.6 * clamp(1 - ((n * 40 + i * 30) % 90) / 90, 0, 1), weight: 800, stroke: 0.06 });
    }
  }, 0.8);
  // the agent at its laptop
  scene(l0.s - 0.4, l2.e + 0.2, now => {
    const a = fadeIn(now, l0.s - 0.4, 0.4);
    const x = cx() - 40 * U, y = cy() + 40 * U;
    let face = "type";
    if (now > guesses.s) face = "?";
    if (now > never.s) face = "x";
    if (now > tov.s) face = "dots";
    if (now > straight.s) face = "happy";
    ctx.save();
    ctx.globalAlpha = a;
    bot(x - 90 * U, y - 20 * U + Math.sin(now * 12) * 3 * U, 130 * U, now, face);
    // laptop
    ctx.translate(x + 60 * U, y + 50 * U);
    ctx.fillStyle = "#2b2733";
    ctx.strokeStyle = C.ink;
    ctx.lineWidth = 4 * U;
    ctx.beginPath(); ctx.moveTo(-110 * U, 0); ctx.lineTo(-80 * U, -150 * U); ctx.lineTo(120 * U, -150 * U); ctx.lineTo(110 * U, 0); ctx.closePath(); ctx.fill(); ctx.stroke();
    ctx.fillStyle = "#3a3544";
    ctx.fillRect(-130 * U, 0, 260 * U, 12 * U);
    ctx.restore();
    // code scrolls past on its screen
    ctx.save();
    ctx.globalAlpha = a;
    ctx.beginPath(); ctx.rect(x - 10 * U, y - 92 * U, 180 * U, 128 * U); ctx.clip();
    const speed = now > night.s ? 90 : 40;
    for (let i = 0; i < 12; i++) {
      const ly = y - 80 * U + ((i * 16 - now * speed) % 192 + 192) % 192 * U - 10 * U;
      const w = (40 + hash(i * 5.3) * 110) * U;
      ctx.fillStyle = [C.cyan, C.amber, "#9DB2F5", C.foam][i % 4];
      ctx.globalAlpha = a * 0.7;
      ctx.fillRect(x + (i % 3) * 12 * U, ly, w, 6 * U);
    }
    ctx.restore();
    // guesses: question marks; never right: red crosses
    if (now > guesses.s && now < tov.s) {
      for (let i = 0; i < 6; i++) {
        const t0 = guesses.s + i * 0.28;
        if (now < t0) continue;
        const k = (now - t0) / 1.4;
        if (k > 1) continue;
        const qx = x - 90 * U + (hash(i * 4.4) - 0.5) * 260 * U, qy = y - 120 * U - k * 120 * U;
        bigText("?", qx, qy, (40 + hash(i) * 30) * U, C.cyan, { a: 1 - k, rot: (hash(i * 2) - 0.5) * 0.6, scale: back(k * 5), stroke: 0.1 });
      }
    }
  }, 0.4);
  on(never.s, () => floatText("✗", cx() - 180 * U, cy() - 80 * U, C.red, 70, 1.1));
  on(right.s, () => { floatText("✗", cx() + 140 * U, cy() - 110 * U, C.red, 90, 1.1); shake(6); });
  // Tov tells it straight
  scene(tov.s - 0.3, l2.e + 0.3, now => {
    const k = out3((now - tov.s + 0.3) / 0.5);
    const bx = lerp(Wd + 120 * U, cx() + 230 * U, k), by = cy() - 110 * U + Math.sin(now * 9) * 6 * U;
    bird(bx, by, 130 * U, now, { flip: true });
    if (now > straight.s - 0.1) {
      const k2 = back((now - straight.s + 0.1) / 0.3);
      ctx.save();
      ctx.translate(bx - 120 * U, by - 70 * U);
      ctx.scale(k2, k2);
      ctx.fillStyle = C.foam;
      ctx.strokeStyle = C.ink;
      ctx.lineWidth = 4 * U;
      roundRect(-150 * U, -36 * U, 220 * U, 64 * U, 18 * U); ctx.fill(); ctx.stroke();
      ctx.beginPath(); ctx.moveTo(20 * U, 26 * U); ctx.lineTo(60 * U, 54 * U); ctx.lineTo(46 * U, 26 * U); ctx.fill();
      ctx.fillStyle = C.ink;
      ctx.font = `800 ${20 * U}px ${FONT}`;
      ctx.textAlign = "center"; ctx.textBaseline = "middle";
      ctx.fillText("line 4: add try", -40 * U, -4 * U);
      ctx.restore();
    }
  }, 0.3);
  // here's the error, here's the fix, and the build goes through
  scene(l3.s - 0.2, Math.min(l3.e + 0.7, until(l3.i) - 0.1), now => {
    const a = fadeIn(now, l3.s - 0.2, 0.3);
    const w = Math.min(600 * U, Wd - 32), h = 210 * U;
    const box = panel(cx() - w / 2, cy() - h / 2 - 10 * U + (1 - out3(a)) * 30 * U, w, h, "tov check --json", a);
    const fs = Math.max(12, 19 * U);
    const row = (i, text, col, at) => {
      if (now < at) return;
      const k = clamp((now - at) / 0.15, 0, 1);
      mono(text, box.x + (1 - k) * 20 * U, box.y + i * fs * 1.7, fs, col, a * k);
    };
    row(0, "✗ error T0831  `new URL` can throw", C.red, err.s);
    row(1, "→ fix (safe)   insert \"try \" at 4:17", C.amber, fix.s);
    row(2, "$ tov build hello.tov", C.foam, W(b + 3, 8).s);
    row(3, "✓ build: hello  (42 ms)", C.green, through.s);
  }, 0.3);
  on(through.s, () => { confetti(36, cx(), cy() + 40 * U, 0.9); ring(cx(), cy(), C.green, 300, 0.6, 8); });
}

// ---- verse two: no any, no null, no ==; a try; 42 ms; poor old Rust
function verse2(b) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  // the banned words, each struck off as it's sung
  const banned = [[W(b, 1), "any", -1], [W(b, 3), "null", 0], [W(b, 6), "==", 1]];
  for (const [w, text, i] of banned) {
    const strike = i === 1 ? l0.e : w.e;
    scene(w.s, l0.e + 0.3, now => {
      const pop = back((now - w.s) / 0.25);
      const gap = Math.min(250 * U, Wd * 0.32), x = cx() + i * gap, y = cy() - 10 * U;
      const gone = clamp((now - l0.e) / 0.3, 0, 1);
      ctx.save();
      ctx.globalAlpha = 1 - gone;
      ctx.translate(x, y + gone * 80 * U);
      ctx.rotate(gone * 0.6 * (i || 1));
      ctx.scale(pop * Math.min(1, gap / (230 * U)), pop * Math.min(1, gap / (230 * U)));
      ctx.fillStyle = "#1a1720";
      ctx.strokeStyle = "rgba(255,243,220,.25)";
      ctx.lineWidth = 2;
      roundRect(-100 * U, -60 * U, 200 * U, 120 * U, 18 * U); ctx.fill(); ctx.stroke();
      mono(text, 0, 0, 56 * U, C.cyan, 1, "center");
      const sk = clamp((now - strike) / 0.15, 0, 1);
      if (sk > 0) {
        ctx.strokeStyle = C.red;
        ctx.lineWidth = 12 * U;
        ctx.lineCap = "round";
        ctx.beginPath(); ctx.moveTo(-80 * U, 40 * U); ctx.lineTo(lerp(-80, 80, sk) * U, lerp(40, -40, sk) * U); ctx.stroke();
      }
      ctx.restore();
    }, 0.01);
    on(strike, () => { sparks(10, cx() + i * Math.min(250 * U, Wd * 0.32), cy() - 10 * U, C.red); shake(5); });
  }
  // put a try on the call
  const tryW = W(b + 1, 2), call = W(b + 1, 5), fine = W(b + 1, 10);
  scene(l1.s - 0.1, l2.s - 0.3, now => {
    const a = fadeIn(now, l1.s - 0.1, 0.25);
    const fs = Math.max(16, 34 * U);
    ctx.font = `700 ${fs}px ${MONO}`;
    const before = "const res = ", after = "fetch(url)";
    const cw = ctx.measureText("m").width;
    const k = now > tryW.s ? back((now - tryW.s) / 0.3) : 0;
    const total = (before.length + after.length) * cw + k * cw * 4;
    let x = cx() - total / 2;
    const y = cy() + Math.sin(now * 3) * 4 * U;
    mono(before, x, y, fs, C.foam, a); x += before.length * cw;
    if (k > 0) {
      ctx.save();
      ctx.translate(x + cw * 2, y - (1 - clamp(k, 0, 1)) * 120 * U);
      ctx.fillStyle = "rgba(228,169,90,.25)";
      roundRect(-cw * 2 - 4, -fs * 0.75, cw * 3.6 + 8, fs * 1.5, 8);
      ctx.fill();
      ctx.restore();
      mono("try", x, y - (1 - clamp(k, 0, 1)) * 120 * U, fs, C.amber, a);
    }
    x += k * cw * 4;
    mono(after, x, y, fs, now > call.s ? C.green : C.foam, a);
    if (now > fine.s) {
      const k2 = clamp((now - fine.s) / 0.35, 0, 1);
      check(cx(), y + 90 * U, 70 * U, C.green, k2);
    }
  }, 0.2);
  on(tryW.s + 0.1, () => sparks(16, cx(), cy(), C.amber));
  on(fine.s, () => ring(cx(), cy() + 90 * U, C.green, 140, 0.5, 6));
  // forty-two milliseconds: a stopwatch
  const ms = W(b + 2, 1), done = W(b + 2, 6);
  scene(l2.s - 0.1, l3.s - 0.15, now => {
    const a = fadeIn(now, l2.s - 0.1, 0.2);
    const k = clamp((now - l2.s) / (ms.e - l2.s), 0, 1);
    const x = cx(), y = cy();
    const r = 110 * U;
    ctx.save();
    ctx.globalAlpha = a;
    drawGlow("amber", x, y, r * 2.4, 0.4);
    ctx.fillStyle = "#1a1720";
    ctx.strokeStyle = C.foam;
    ctx.lineWidth = 8 * U;
    ctx.beginPath(); ctx.arc(x, y, r, 0, TAU); ctx.fill(); ctx.stroke();
    ctx.fillStyle = C.foam;
    roundRect(x - 18 * U, y - r - 30 * U, 36 * U, 22 * U, 6 * U); ctx.fill();
    ctx.strokeStyle = C.amber;
    ctx.lineWidth = 12 * U;
    ctx.lineCap = "round";
    ctx.beginPath(); ctx.arc(x, y, r - 18 * U, -Math.PI / 2, -Math.PI / 2 + TAU * k * 0.42 * 2.38); ctx.stroke();
    ctx.restore();
    bigText(`${Math.round(42 * out3(k))}`, x, y - 8 * U, 84 * U, C.amber, { a, stroke: 0.06 });
    mono("ms", x, y + 46 * U, 22 * U, C.foam, a, "center");
    if (now > done.s) {
      const k2 = back((now - done.s) / 0.3);
      bigText("BUILD DONE", x, y + r + 50 * U, 44 * U, C.green, { a, scale: k2, rot: -0.04, stroke: 0.12 });
    }
  }, 0.2);
  on(done.s, () => { confetti(30, cx(), cy(), 1); shake(8); });
  // poor old Rust's still compiling, it's missed all the fun
  const comp = W(b + 3, 4), fun = W(b + 3, 9);
  scene(l3.s - 0.1, until(l3.i) - 0.2, now => {
    const a = fadeIn(now, l3.s - 0.1, 0.25);
    const x = cx(), y = cy() + 20 * U;
    const sad = now > fun.s ? 1 : 0.5;
    crab(x - 170 * U, y + 30 * U, 120 * U, now, sad, 0.2);
    const w = Math.min(380 * U, Wd * 0.5), h = 150 * U;
    const box = panel(x - 70 * U, y - 70 * U, w, h, "cargo build", a);
    const crates = ["serde v1.0.219", "syn v2.0.101", "tokio v1.45.0", "proc-macro2 v1.0.95", "hyper v1.6.0"];
    const i = Math.floor(Math.max(0, now - l3.s) * 3) % crates.length;
    mono(`Compiling ${crates[i]}`, box.x, box.y, Math.max(11, 15 * U), "#8FD3AE", a);
    const prog = 0.02 + clamp((now - l3.s) / 20, 0, 1) * 0.1;
    ctx.globalAlpha = a;
    ctx.fillStyle = "rgba(255,243,220,.14)";
    ctx.fillRect(box.x, box.y + 28 * U, box.w, 12 * U);
    ctx.fillStyle = C.rust;
    ctx.fillRect(box.x, box.y + 28 * U, box.w * prog, 12 * U);
    ctx.globalAlpha = 1;
    mono(`Building [${Math.round(prog * 419)}/419]`, box.x, box.y + 62 * U, Math.max(11, 14 * U), "rgba(255,243,220,.55)", a);
    // the spinner
    ctx.save();
    ctx.translate(box.x + box.w - 14 * U, box.y + 62 * U);
    ctx.rotate(now * 4);
    ctx.strokeStyle = C.amber;
    ctx.lineWidth = 3 * U;
    ctx.beginPath(); ctx.arc(0, 0, 9 * U, 0, Math.PI * 1.4); ctx.stroke();
    ctx.restore();
    if (now > fun.s) {
      // the party it missed, going on somewhere else
      const k = clamp((now - fun.s) / 0.8, 0, 1);
      bigText("🎉", cx() + 300 * U * narrow(), cy() - 120 * U, 60 * U, C.foam, { a: a * (1 - k * 0.5), scale: back(k * 2), stroke: 0 });
    }
  }, 0.25);
  on(comp.s, () => floatText("still compiling…", cx() - 170 * U, cy() - 80 * U, "#FF8A5C", 26, 1.6));
}

// ---- the bridge: tap tap tap, bleep bleep bleep, 5 4 3 2 1, DING
const KEYROWS = ["QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"];
const keyLit = new Map();
function lightKey(ch, t) { keyLit.set(ch.toUpperCase(), t); }
function bridge(b) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  // the keyboard
  scene(l0.s - 0.1, l1.e + 0.2, now => {
    const a = fadeIn(now, l0.s - 0.1, 0.2);
    const key = Math.min(64 * U, (Wd - 40) / 11);
    const gap = key * 0.14;
    const top = cy() - key * 0.6;
    ctx.save();
    ctx.globalAlpha = a;
    KEYROWS.forEach((row, r) => {
      const rw = row.length * (key + gap) - gap;
      const x0 = cx() - rw / 2 + r * key * 0.25;
      [...row].forEach((ch, i) => {
        const lit = keyLit.get(ch);
        const k = lit === undefined ? 1 : clamp((now - lit) / 0.35, 0, 1);
        const press = 1 - k;
        const x = x0 + i * (key + gap), y = top + r * (key + gap) + press * 5 * U;
        ctx.fillStyle = "rgba(0,0,0,.45)";
        roundRect(x, top + r * (key + gap) + 6 * U, key, key, 10 * U); ctx.fill();
        ctx.fillStyle = press > 0 ? mix(hex("#2a2533"), hex(C.pink), press) : "#2a2533";
        roundRect(x, y, key, key, 10 * U); ctx.fill();
        ctx.strokeStyle = "rgba(255,243,220,.2)";
        ctx.lineWidth = 1.5;
        ctx.stroke();
        if (press > 0) drawGlow("pink", x + key / 2, y + key / 2, key * 1.2, press * 0.6);
        mono(ch, x + key / 2, y + key / 2, key * 0.38, press > 0.3 ? "#fff" : "rgba(255,243,220,.7)", 1, "center");
      });
    });
    ctx.restore();
  }, 0.3);
  // each "tap" presses keys; "keys" presses a flurry
  for (let i = 0; i < 3; i++) {
    const w = W(b, i);
    on(w.s, () => {
      for (let j = 0; j < 4; j++) lightKey("QWERTYUIOPASDFGHJKLZXCVBNM"[Math.floor(Math.random() * 26)], w.s);
      floatText("TAP!", cx() + (i - 1) * 220 * U, cy() - 120 * U, C.pink, 46, 0.7);
      shake(5);
    });
    target(w.s, "TAP");
  }
  on(W(b, 6).s, () => { for (const ch of "TOVAGENTS") lightKey(ch, W(b, 6).s); });
  // bleep, bleep, bleep
  for (let i = 0; i < 3; i++) {
    const w = W(b + 1, i);
    on(w.s, () => { floatText("BLEEP", cx() + (i - 1) * 230 * U, cy() - 150 * U - i * 10 * U, C.cyan, 44, 0.8); pixels(18, cx() + (i - 1) * 230 * U, cy() - 150 * U); });
    target(w.s, "BLEEP");
  }
  // check the JSON please
  const json = W(b + 1, 5), please = W(b + 1, 6);
  scene(json.s - 0.1, l1.e + 0.4, now => {
    const a = fadeIn(now, json.s - 0.1, 0.2);
    const fs = Math.max(14, 30 * U);
    const y = cy() - 160 * U;
    mono(`{ "build": "ok" }`, cx(), y, fs, C.foam, a, "center");
    if (now > please.s) check(cx() + 190 * U, y, 40 * U, C.green, clamp((now - please.s) / 0.3, 0, 1));
  }, 0.2);
  // five, four, three, two, one
  const cols = [C.cyan, C.green, C.amber, C.pink, C.red];
  for (let i = 0; i < 5; i++) {
    const w = W(b + 2, i);
    stamp(w.s, ["5", "4", "3", "2", "1"][i], { size: 280 + i * 30, col: cols[i], glow: ["cyan", "white", "amber", "pink", "pink"][i], rot: (i % 2 ? 0.06 : -0.06), life: 0.55, shake: 8 + i * 4 });
    target(w.s, "!");
    on(w.s, () => flash(0.08 + i * 0.04, cols[i]));
  }
  // DING!
  const ding = W(b + 3, 0), done = W(b + 3, 4);
  target(ding.s, "DING");
  on(ding.s, () => { flash(0.55); shake(28); confetti(140, cx(), cy(), 1.6); ring(cx(), cy(), "#fff", 500, 0.9, 16); sparks(40, cx(), cy(), C.amberHi, 1.5); });
  scene(ding.s, l3.e + 0.6, now => {
    const k = now - ding.s;
    const swing = Math.sin(k * 14) * 0.5 * Math.exp(-k * 2.5);
    const x = cx(), y = cy() - 70 * U;
    // rays
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(k * 0.6);
    ctx.globalAlpha = clamp(1 - k / 2.5, 0, 1) * 0.35;
    ctx.fillStyle = C.amberHi;
    for (let i = 0; i < 14; i++) {
      ctx.rotate(TAU / 14);
      ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(-40 * U, -Math.max(Wd, Ht)); ctx.lineTo(40 * U, -Math.max(Wd, Ht)); ctx.closePath(); ctx.fill();
    }
    ctx.restore();
    // the bell
    ctx.save();
    ctx.translate(x, y - 70 * U);
    ctx.rotate(swing);
    const s = U * (1 + Math.exp(-k * 6) * 0.3);
    ctx.scale(s, s);
    ctx.fillStyle = C.amber;
    ctx.strokeStyle = C.ink;
    ctx.lineWidth = 7;
    ctx.beginPath();
    ctx.moveTo(-80, 110); ctx.quadraticCurveTo(-70, 20, -50, -10); ctx.quadraticCurveTo(0, -70, 50, -10); ctx.quadraticCurveTo(70, 20, 80, 110); ctx.closePath();
    ctx.fill(); ctx.stroke();
    ctx.fillStyle = "rgba(255,255,255,.35)";
    ctx.beginPath(); ctx.ellipse(-30, 20, 10, 36, 0.2, 0, TAU); ctx.fill();
    ctx.fillStyle = C.ink;
    ctx.beginPath(); ctx.arc(0, 122, 16, 0, TAU); ctx.fill();
    ctx.fillStyle = C.amber;
    ctx.beginPath(); ctx.arc(0, -46, 12, 0, TAU); ctx.fill(); ctx.stroke();
    ctx.restore();
    bigText("DING!", x, y + 120 * U, 110 * U, "#fff", { scale: back(k / 0.2), rot: -0.06, stroke: 0.1 });
    if (now > W(b + 3, 3).s) bigText("BUILD ✓", x, y + 210 * U, 46 * U, C.green, { scale: back((now - W(b + 3, 3).s) / 0.3), stroke: 0.12 });
  }, 0.01);
  // the tension: the arcade gets brighter and faster until the DING
  bridgeSpan = [l0.s - 1, ding.s];
}
let bridgeSpan = [0, 0];

// ---- the outro: Tov! Tov! ... curl it, pipe it, off you go, Tov dot S-H, OI!
function outro(b) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  stamp(W(b, 0).s, "TOV!", { rot: -0.1, dx: -150 });
  stamp(W(b, 1).s, "TOV!", { rot: 0.1, dx: 150 });
  target(W(b, 0).s, "TOV"); target(W(b, 1).s, "TOV");
  race(W(b, 2).s, l1.s + 0.2);
  stamp(W(b + 1, 0).s, "TOV!", { rot: -0.12, dx: -220, size: 170 });
  stamp(W(b + 1, 1).s, "TOV!", { rot: 0.0, dx: 0, size: 190 });
  stamp(W(b + 1, 2).s, "TOV!", { rot: 0.12, dx: 220, size: 210 });
  for (let i = 0; i < 3; i++) target(W(b + 1, i).s, "TOV");
  const bust = W(b + 1, 4);
  stamp(bust.s, "BUST!", { size: 180, col: C.pink, glow: "pink", rot: -0.06, shake: 22 });
  on(bust.s, () => confetti(60, cx(), cy(), 1.3));
  // curl it, pipe it, off you go
  const curl = W(b + 2, 0), pipe = W(b + 2, 2), off = W(b + 2, 4), go = W(b + 2, 6);
  scene(l2.s - 0.2, l3.s + 0.2, now => {
    const a = fadeIn(now, l2.s - 0.2, 0.25);
    const w = Math.min(720 * U, Wd - 32), h = 170 * U;
    const box = panel(cx() - w / 2, cy() - h / 2, w, h, "zsh", a);
    const fs = Math.max(11, Math.min(22 * U, (w - 44 * U) / 46));
    const first = "curl -fsSL https://tov.sh/install.sh", second = " | sh";
    const k1 = clamp((now - curl.s) / Math.max(0.2, pipe.s - curl.s), 0, 1);
    const k2 = clamp((now - pipe.s) / Math.max(0.15, pipe.e - pipe.s), 0, 1);
    const shown = first.slice(0, Math.floor(first.length * k1)) + second.slice(0, Math.floor(second.length * k2));
    ctx.font = `600 ${fs}px ${MONO}`;
    const cw = ctx.measureText("m").width;
    mono("$ " + shown, box.x, box.y, fs, C.foam, a);
    if (Math.floor(now * 4) % 2 === 0) mono("▌", box.x + cw * (shown.length + 2), box.y, fs, C.amber, a);
    if (now > off.s) mono("installing tov… ", box.x, box.y + fs * 1.8, fs, "rgba(255,243,220,.6)", a);
    if (now > go.s) mono("✓ tov 0.0.1 is ready. off you go!", box.x, box.y + fs * 3.4, fs, C.green, a);
    // and off the bird goes
    if (now > off.s) {
      const k = (now - off.s) / 1.4;
      const bx = lerp(cx() - 200 * U, Wd + 200 * U, inOut(k)), by = cy() - 140 * U - Math.sin(k * Math.PI) * 60 * U;
      speedLines(bx - 40 * U, by, 300 * U, now, 0.8);
      bird(bx, by, 140 * U, now, { rot: -0.15 });
    }
  }, 0.25);
  // Tov dot S-H... OI!
  const tov = W(b + 3, 0), dot = W(b + 3, 1), sh = W(b + 3, 2), oi = W(b + 3, 3);
  target(tov.s, "TOV");
  scene(tov.s, oi.s, now => {
    const parts = [["TOV", tov.s], [".", dot.s], ["S", sh.s], ["H", sh.s + Math.min(0.5, (sh.e - sh.s) / 2)]];
    const size = Math.min(200 * U, Wd / 4.2);
    ctx.font = `900 ${size}px ${FONT}`;
    const widths = parts.map(([p]) => ctx.measureText(p).width);
    const total = widths.reduce((a, b) => a + b, 0);
    let x = cx() - total / 2;
    parts.forEach(([p, at], i) => {
      const k = clamp((now - at) / 0.25, 0, 1);
      if (k > 0) bigText(p, x + widths[i] / 2, cy() - 30 * U, size, i === 0 ? C.amber : C.foam, { scale: back(k), stroke: 0.07, a: k });
      x += widths[i];
    });
  }, 0.01);
  for (const at of [tov.s, dot.s, sh.s]) on(at, () => { shake(8); sparks(12, cx(), cy() - 30 * U, C.amber); });
  stamp(oi.s, "OI!", { size: 320, rot: -0.1, shake: 30, sparks: 40, life: 1.6 });
  target(oi.s, "OI!");
  on(oi.s, () => { flash(0.4, C.amber); confetti(180, cx(), cy(), 1.8); for (let i = 0; i < 6; i++) foam(10, Wd * (i + 0.5) / 6, Ht * 0.7); });
  scene(oi.s, oi.s + 6, now => { if (Math.random() < 0.6) rainConfetti(2); }, 0.01);
}

function cueAll() {
  for (const s of sections) {
    const b = s.lines[0].i;
    if (s.kind === "intro") intro(b);
    else if (s.kind === "chorus") chorus(b, false);
    else if (s.kind === "final") chorus(b, true);
    else if (s.kind === "verse1") verse1(b);
    else if (s.kind === "verse2") verse2(b);
    else if (s.kind === "bridge") bridge(b);
    else if (s.kind === "outro") outro(b);
  }
  targets.sort((a, b) => a.t - b.t);
  triggers.sort((a, b) => a.t - b.t);
}

// ---------------------------------------------------------------- the stage: backdrops

const STARS = Array.from({ length: 140 }, (_, i) => ({ x: hash(i * 1.3), y: hash(i * 2.7) * 0.75, r: 0.6 + hash(i * 4.1) * 1.8, tw: hash(i * 5.9) * TAU }));
const BOKEH = Array.from({ length: 18 }, (_, i) => ({ x: hash(i * 3.1), y: hash(i * 7.7), r: 30 + hash(i * 1.9) * 90, sp: 0.02 + hash(i * 6.2) * 0.04, col: ["amber", "pink", "cyan", "white", "violet"][i % 5] }));

function backdrop(name, a, t, pulse, tension) {
  if (a <= 0.01) return;
  ctx.save();
  ctx.globalAlpha = a;
  if (name === "pub" || name === "burst") {
    for (const b of BOKEH) {
      const x = ((b.x + t * b.sp) % 1.2 - 0.1) * Wd, y = (b.y * 0.8 + Math.sin(t * 0.5 + b.x * 9) * 0.03) * Ht;
      drawGlow(b.col, x, y, b.r * U * (1 + pulse * 0.25), a * (name === "pub" ? 0.3 : 0.22));
    }
    ctx.globalAlpha = a;
  }
  if (name === "burst") {
    ctx.save();
    ctx.translate(cx(), cy() - 30 * U);
    ctx.rotate(t * 0.12);
    ctx.fillStyle = `rgba(255,240,180,${0.1 + pulse * 0.12})`;
    const R = Math.hypot(Wd, Ht);
    for (let i = 0; i < 18; i++) {
      ctx.rotate(TAU / 18);
      ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(-R * 0.09, -R); ctx.lineTo(R * 0.09, -R); ctx.closePath(); ctx.fill();
    }
    ctx.restore();
    drawGlow("white", cx(), cy() - 30 * U, 380 * U * (1 + pulse * 0.25), 0.3 * a);
  }
  if (name === "night") {
    for (const s of STARS) {
      ctx.globalAlpha = a * (0.4 + 0.6 * (Math.sin(t * 2 + s.tw) + 1) / 2);
      ctx.fillStyle = "#fff";
      ctx.fillRect(s.x * Wd, s.y * Ht, s.r * U * 1.6, s.r * U * 1.6);
    }
  }
  if (name === "grid") {
    const g = 56 * U, off = (t * 30 * U) % g;
    ctx.strokeStyle = `rgba(255,255,255,${0.12 + pulse * 0.12})`;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = -off; x < Wd; x += g) { ctx.moveTo(x, 0); ctx.lineTo(x, Ht); }
    for (let y = -off; y < Ht; y += g) { ctx.moveTo(0, y); ctx.lineTo(Wd, y); }
    ctx.stroke();
  }
  if (name === "arcade") {
    const hz = Ht * 0.58;
    // the sun
    const sr = 170 * U;
    if (!sunGrad) {
      sunGrad = ctx.createLinearGradient(0, hz - sr, 0, hz);
      sunGrad.addColorStop(0, C.yellow);
      sunGrad.addColorStop(1, C.pink);
    }
    ctx.fillStyle = sunGrad;
    ctx.beginPath(); ctx.arc(cx(), hz, sr * (1 + pulse * 0.05), Math.PI, 0); ctx.fill();
    ctx.fillStyle = "#4B00A8";
    for (let i = 0; i < 6; i++) ctx.fillRect(cx() - sr * 1.1, hz - sr * 0.1 - i * sr * 0.14, sr * 2.2, (2 + i * 1.2) * U);
    // the floor
    ctx.fillStyle = "rgba(13,0,38,.7)";
    ctx.fillRect(0, hz, Wd, Ht - hz);
    ctx.strokeStyle = mix(hex(C.pink), hex(C.cyan), tension);
    ctx.globalAlpha = a * (0.5 + pulse * 0.4);
    ctx.lineWidth = 2;
    ctx.beginPath();
    const speed = 0.6 + tension * 3;
    for (let i = 0; i < 14; i++) {
      const z = ((i / 14 + t * speed * 0.25) % 1);
      const y = hz + Math.pow(z, 2.2) * (Ht - hz);
      ctx.moveTo(0, y); ctx.lineTo(Wd, y);
    }
    for (let i = -12; i <= 12; i++) {
      ctx.moveTo(cx() + i * 18 * U, hz);
      ctx.lineTo(cx() + i * 260 * U, Ht);
    }
    ctx.stroke();
  }
  ctx.restore();
}

// the crowd at the bottom, pints up, on the beat (choruses)
function crowd(t, a, beat, big) {
  if (a <= 0.01) return;
  const n = Math.max(6, Math.round(Wd / (90 * U)));
  const base = Ht + 10 * U;
  ctx.save();
  ctx.globalAlpha = a;
  for (let i = 0; i < n; i++) {
    const x = (i + 0.5) * Wd / n + (hash(i) - 0.5) * 30 * U;
    const hop = Math.abs(Math.sin((beat + hash(i * 3) * 0.2) * Math.PI)) * (big ? 22 : 12) * U;
    const hr = (22 + hash(i * 2) * 9) * U, y = base - 100 * U - hash(i * 5) * 34 * U - hop;
    ctx.fillStyle = "rgba(30,16,51,.92)";
    ctx.beginPath(); ctx.arc(x, y, hr, 0, TAU); ctx.fill();
    roundRect(x - hr * 1.7, y + hr * 0.8, hr * 3.4, 200 * U, hr); ctx.fill();
    if (i % 2 === 0 || big) {
      const side = i % 4 < 2 ? 1 : -1;
      const sway = Math.sin(beat * Math.PI + i) * 0.25;
      const hx = x + side * hr * 1.6 + sway * 20 * U, hy = y - hr * 1.6;
      ctx.strokeStyle = "rgba(30,16,51,.92)";
      ctx.lineWidth = hr * 0.6;
      ctx.lineCap = "round";
      ctx.beginPath(); ctx.moveTo(x + side * hr * 1.2, y + hr * 1.2); ctx.lineTo(hx, hy); ctx.stroke();
      pint(hx, hy + 6 * U, 46 * U, sway * 0.5);
    }
  }
  ctx.restore();
}

// ---------------------------------------------------------------- the frame

let raf = 0, lastT = -1, lastNow = 0, curLine = -1, nextLine = -1, prevLine = -1;
// (a frame that runs long counts against the stage: enough of them, and it drops its resolution
// and its particles, so a slow machine keeps the beat)
let slow = 0;

function frame(now) {
  raf = requestAnimationFrame(frame);
  const t = songTime(now);
  const gap = now - lastNow;
  const dt = Math.min(0.05, Math.max(0, gap / 1000));
  lastNow = now;

  // fire what the song passed (but not what a seek skipped)
  if (lastT >= 0 && t > lastT && t - lastT < 0.3) {
    for (const g of triggers) if (g.t > lastT && g.t <= t) g.fn();
  }
  lastT = t;

  lyrics(t);
  hud(t);
  draw(t, dt);

  const work = performance.now() - now;
  slow = work > 9 || (gap > 45 && gap < 250) ? slow + 1 : Math.max(0, slow - 0.5);
  if (slow > 45 && quality > 0.5) {
    quality = Math.max(0.5, quality * 0.75);
    MAXP = Math.max(120, Math.round(MAXP * 0.6));
    slow = 0;
    resize();
  }
}

let skySec = -1, skyOn = 0;
function sky(si) {
  if (si === skySec) return;
  skySec = si;
  const th = THEMES[sections[si].kind];
  skyOn = 1 - skyOn;
  els.skies[skyOn].style.background = `linear-gradient(to bottom, rgba(20,0,40,0) 50%, rgba(20,0,40,.5)), radial-gradient(ellipse at 50% 42%, rgba(0,0,0,0) 45%, rgba(20,0,40,.4)), linear-gradient(to bottom, ${th.sky[0]}, ${th.sky[1]} 55%, ${th.sky[2]})`;
  els.skies[skyOn].classList.add("on");
  els.skies[1 - skyOn].classList.remove("on");
  els.sec.style.setProperty("--tag", th.tag);
  root.classList.toggle("is-arcade", sections[si].kind === "bridge");
}

let flashShown = 0;
function draw(t, dt) {
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, cv.width, cv.height);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  const si = sectionAt(t), sec = sections[si];
  sky(si);
  const th = THEMES[sec.kind], prev = si > 0 ? THEMES[sections[si - 1].kind] : th;
  const fk = clamp((t - sec.at) / 1.2, 0, 1);
  const beat = (t - BEAT0) / BEAT;
  const frac = beat - Math.floor(beat);
  const playing = !audio.paused;
  const bass = env(t, 0);
  const pulse = playing ? Math.max(bass * 0.9, Math.pow(1 - frac, 5) * 0.5) : 0;
  const tension = bridgeSpan[1] > bridgeSpan[0] ? clamp((t - bridgeSpan[0]) / (bridgeSpan[1] - bridgeSpan[0]), 0, 1) : 0;

  // shake and punch everything on the stage
  shakeAmt *= Math.exp(-dt * 9);
  zoomAmt *= Math.exp(-dt * 10);
  ctx.save();
  if (shakeAmt > 0.3) ctx.translate((Math.random() - 0.5) * shakeAmt * U * 1.6, (Math.random() - 0.5) * shakeAmt * U * 1.6);
  if (zoomAmt > 0.002) {
    ctx.translate(cx(), cy());
    ctx.scale(1 + zoomAmt, 1 + zoomAmt);
    ctx.translate(-cx(), -cy());
  }

  if (prev.bg !== th.bg) backdrop(prev.bg, 1 - fk, t, pulse, tension);
  backdrop(th.bg, prev.bg !== th.bg ? fk : 1, t, pulse, sec.kind === "bridge" ? tension : 0);

  const loud = sec.kind === "chorus" || sec.kind === "final" || sec.kind === "outro";
  const wasLoud = si > 0 && ["chorus", "final", "outro"].includes(sections[si - 1].kind);
  crowd(t, (loud ? fk : 0) + (wasLoud ? 1 - fk : 0), beat, sec.kind !== "chorus");

  for (const c of scenes) {
    if (t < c.s - c.fade || t > c.e + c.fade) continue;
    const a = Math.min(1, (t - c.s + c.fade) / c.fade, (c.e + c.fade - t) / c.fade);
    if (a <= 0) continue;
    ctx.save();
    c.draw(t, a);
    ctx.restore();
  }

  stepParticles(dt);
  drawParticles();
  ctx.restore();

  // flashes are a layer of their own (a canvas-wide fill each frame would cost more)
  flashAmt *= Math.exp(-dt * 8);
  if (flashAmt < 0.01) flashAmt = 0;
  if (Math.abs(flashAmt - flashShown) > 0.01 || (flashAmt === 0 && flashShown !== 0)) {
    els.flash.style.display = flashAmt > 0 ? "block" : "none";
    els.flash.style.opacity = flashAmt.toFixed(2);
    flashShown = flashAmt;
  }
}

// ---------------------------------------------------------------- the lyrics

function setLine(i, cls) {
  if (i < 0 || i >= lines.length) return;
  lines[i].el.className = "ts-l " + cls;
}

function lyrics(t) {
  let cur = -1;
  for (let i = 0; i < lines.length; i++) {
    if (t < lines[i].show) break;
    if (t < lines[i].hide) { cur = i; break; }
  }
  // in a gap, show what's coming as the next line
  let next = -1;
  if (cur >= 0) next = cur + 1 < lines.length ? cur + 1 : -1;
  else { for (let i = 0; i < lines.length; i++) if (lines[i].show > t) { next = i; break; } }
  const prev = cur >= 0 ? cur - 1 : next - 1;
  if (cur !== curLine || next !== nextLine) {
    for (const i of [curLine, nextLine, prevLine]) if (i >= 0) setLine(i, "");
    setLine(prev, "is-prev");
    setLine(next, "is-next");
    setLine(cur, "is-cur");
    curLine = cur; nextLine = next; prevLine = prev;
    lift();
  }
  // the words: lit as they're sung, the one being sung filling left to right
  if (cur >= 0) {
    for (const w of lines[cur].words) {
      const p = t <= w.s ? 0 : t >= w.e ? 1 : (t - w.s) / (w.e - w.s);
      if (p !== w.p) {
        w.p = p;
        w.el.style.setProperty("--p", p.toFixed(3));
        const sing = p > 0 && p < 1;
        if (sing !== w.on) { w.on = sing; w.el.classList.toggle("is-on", sing); }
        if ((p >= 1) !== w.done) { w.done = p >= 1; w.el.classList.toggle("is-done", w.done); }
      }
    }
  }
  // (lines no longer shown are reset, so a seek back finds them unlit)
  for (const i of [prevLine, nextLine]) {
    if (i < 0) continue;
    for (const w of lines[i].words) {
      const p = i === prevLine ? 1 : 0;
      if (w.p !== p) {
        w.p = p; w.on = false; w.done = p >= 1;
        w.el.style.setProperty("--p", String(p));
        w.el.classList.remove("is-on");
        w.el.classList.toggle("is-done", w.done);
      }
    }
  }
}

function resetLyrics() {
  for (const l of lines) {
    l.el.className = "ts-l";
    for (const w of l.words) { w.p = 0; w.on = false; w.done = false; w.el.style.setProperty("--p", "0"); w.el.classList.remove("is-on", "is-done"); }
  }
  curLine = nextLine = prevLine = -1;
}

// ---------------------------------------------------------------- taps

// A tap (or a click, or a key) on a big word blows it up; anywhere else, it clinks a pint or
// throws a letter.
function shout(x, y, key) {
  if (!root || root.hidden || audio.paused) return;
  const t = songTime(performance.now());
  const g = targets.find(g => !g.popped && Math.abs(t - g.t) < 0.3);
  if (g) {
    g.popped = true;
    const sx = cx(), sy = cy() - 40 * U;
    const col = CONF[Math.floor(Math.random() * CONF.length)];
    floatText(`${g.label}!`, sx + (Math.random() - 0.5) * 300 * U * narrow(), sy - 140 * U, col, 54, 0.9);
    confetti(40, sx, sy, 1.1);
    ring(sx, sy, col, 320, 0.5, 14);
    shake(10);
    punch(0.04);
    flash(0.12, col);
  } else if (key) {
    lightKey(key, t);
    floatText(key.toUpperCase(), 40 * U + Math.random() * (Wd - 80 * U), Ht * 0.6, CONF[Math.floor(Math.random() * CONF.length)], 36, 0.7);
  } else {
    foam(14, x, y);
    floatText(["cheers!", "clink!", "oi!", "pints!", "tov!"][Math.floor(Math.random() * 5)], x, y - 30 * U, CONF[Math.floor(Math.random() * CONF.length)], 28, 0.8);
  }
}

function resetLive() {
  // a seek: the big words ahead can be blown up again
  const t = audio.currentTime;
  for (const g of targets) g.popped = g.t < t;
  P.length = 0;
  lastT = -1;
}

let lastSec = -2, lastPct = -1;
function hud(t) {
  const si = sectionAt(t);
  const showSec = t > 0.2 ? si : -1;
  if (showSec !== lastSec) {
    els.sec.textContent = showSec >= 0 ? NAMES[sections[si].kind] : "";
    els.sec.style.opacity = showSec >= 0 ? "1" : "0";
    els.sec.classList.remove("swap");
    void els.sec.offsetWidth;
    els.sec.classList.add("swap");
    lastSec = showSec;
  }
  // the hint shows from the intro's end to the first chorus's first TOV
  const firstTov = targets.length > 1 ? targets[1].t : 10;
  const hint = !audio.paused && t > 3.6 && t < firstTov + 0.6;
  if (hint !== els.hint.shown) { els.hint.shown = hint; els.hint.classList.toggle("show", hint); }
  const d = audio.duration || 0;
  const pct = d ? t / d : 0;
  if (Math.abs(pct - lastPct) > 0.0005) {
    els.fill.style.transform = `scaleX(${pct})`;
    els.knob.style.left = `${pct * 100}%`;
    els.time.textContent = `${clock(t)} / ${clock(d)}`;
    els.track.setAttribute("aria-valuenow", String(Math.round(t)));
    lastPct = pct;
  }
}
function clock(s) { s = Math.max(0, Math.floor(s || 0)); return `${Math.floor(s / 60)}:${String(s % 60).padStart(2, "0")}`; }

// ---------------------------------------------------------------- controls

function drawTicks() {
  if (!els.track || !audio.duration) return;
  els.track.setAttribute("aria-valuemax", String(Math.round(audio.duration)));
  els.track.querySelectorAll(".ts-tick").forEach(e => e.remove());
  for (const s of sections.slice(1)) {
    const tick = document.createElement("span");
    tick.className = "ts-tick";
    tick.style.left = `${(s.at / audio.duration) * 100}%`;
    tick.title = NAMES[s.kind];
    els.track.appendChild(tick);
  }
}

function bindTrack() {
  const seekAt = e => {
    const r = els.track.getBoundingClientRect();
    const k = clamp((e.clientX - r.left) / r.width, 0, 1);
    if (audio.duration) audio.currentTime = k * audio.duration;
  };
  els.track.addEventListener("pointerdown", e => {
    e.stopPropagation();
    els.track.setPointerCapture(e.pointerId);
    seekAt(e);
    const move = ev => seekAt(ev);
    const up = () => { els.track.removeEventListener("pointermove", move); els.track.removeEventListener("pointerup", up); };
    els.track.addEventListener("pointermove", move);
    els.track.addEventListener("pointerup", up);
  });
  els.track.addEventListener("keydown", e => {
    if (e.key === "ArrowLeft" || e.key === "ArrowRight") { e.preventDefault(); e.stopPropagation(); skip(e.key === "ArrowLeft" ? -5 : 5); }
  });
}

function skip(by) { if (audio.duration) audio.currentTime = clamp(audio.currentTime + by, 0, audio.duration - 0.1); }

function setPlaying(p) {
  els.play.innerHTML = p ? ICON.pause : ICON.play;
  els.play.setAttribute("aria-label", p ? "Pause" : "Play");
  anchor(audio.currentTime);
}

function toggle() {
  if (audio.paused) begin();
  else audio.pause();
}

function begin() {
  const p = audio.play();
  if (!p) return;
  p.then(() => { els.start.classList.add("away"); setTimeout(() => { els.start.hidden = true; }, 450); })
    // (only a browser that won't play without a tap brings the start card back: a pause that
    // interrupts the play doesn't)
    .catch(err => { if (err && err.name === "NotAllowedError") { els.start.hidden = false; els.start.classList.remove("away"); } });
}

// back to the top: the song, the words, the stage
function restart() {
  audio.pause();
  if (audio.readyState > 0) audio.currentTime = 0;
  anchor(0);
  resetLyrics();
  for (const g of targets) g.popped = false;
  P.length = 0;
  lastT = -1;
  shakeAmt = flashAmt = zoomAmt = 0;
  skySec = lastSec = -2;
  lastPct = -1;
  els.end.hidden = true;
}

function again() {
  restart();
  begin();
}

function finish() {
  els.end.hidden = false;
  els.end.classList.remove("away");
  els.end.querySelector(".ts-again").focus();
}

function onKey(e) {
  if (!root || root.hidden) return;
  if (e.key === "Escape") { e.preventDefault(); close(); return; }
  if (e.target instanceof HTMLButtonElement && (e.key === "Enter" || e.key === " ")) return;
  if (e.metaKey || e.ctrlKey || e.altKey) return;
  if (e.key === " ") { e.preventDefault(); toggle(); return; }
  if (e.key === "ArrowLeft" || e.key === "ArrowRight") { e.preventDefault(); skip(e.key === "ArrowLeft" ? -5 : 5); return; }
  if (e.repeat || e.key.length !== 1) return;
  shout(0, 0, e.key);
}

let opener = null;
function open(from) {
  if (!root) { build(); cueAll(); }
  opener = from || document.activeElement;
  root.hidden = false;
  document.documentElement.style.overflow = "hidden";
  resize();
  // (every opening starts the song from the top)
  restart();
  document.addEventListener("keydown", onKey);
  if (!raf) { lastNow = performance.now(); raf = requestAnimationFrame(frame); }
  els.start.hidden = false;
  els.start.classList.remove("away");
  root.querySelector(".ts-go").focus();
  if (location.hash !== "#singalong") history.replaceState(null, "", "#singalong");
  // (fonts first, so the stage's words are in the page's typeface)
  if (document.fonts) document.fonts.load(`900 100px ${FONT}`).catch(() => {});
  begin();
}

function close() {
  audio.pause();
  root.hidden = true;
  document.documentElement.style.overflow = "";
  document.removeEventListener("keydown", onKey);
  cancelAnimationFrame(raf);
  raf = 0;
  P.length = 0;
  if (location.hash === "#singalong") history.replaceState(null, "", location.pathname + location.search);
  if (opener && opener.focus) opener.focus();
}

window.tovSingalong = open;
})();
