// "Fast with Tov", the singalong: the song, its words lit as they're sung, and a stage that acts
// them out in time. The stage is one canvas (drawn from the song's clock every frame, so seeking
// and pausing just work); the lyrics are text over it. The landing page loads this file when
// someone asks to sing (page.tov) and calls window.tovSingalong().
(() => {
"use strict";

// Each line: its section, and its words, each with the second it starts and ends (the lyrics
// force-aligned to the song's separated vocals, then snapped to where the voice starts).
const LYRICS = [["intro",[["Oi!",0.105,0.798],["Agents!",1.234,2.045],["Pints",2.98,3.447],["up!",3.447,3.647]]],["chorus",[["So",9.339,9.813],["it's",9.813,10.072],["Tov!",10.072,10.925],["Tov!",10.925,11.663],["Faster",11.663,12.267],["than",12.267,12.617],["Rust!",12.617,13.165]]],["chorus",[["Written",13.325,13.918],["like",13.918,14.387],["TypeScript,",14.387,15.061],["it's",15.061,15.255],["Tov",15.255,15.689],["or",15.689,16.123],["bust!",16.123,16.557]]],["chorus",[["Your",16.792,17.041],["agent",17.041,17.742],["writes",17.742,18.119],["it",18.119,18.498],["and",18.498,18.668],["Tov",18.668,19.092],["puts",19.092,19.371],["it",19.371,19.516],["right,",19.516,19.83]]],["chorus",[["Ship",19.83,20.364],["one",20.364,20.833],["little",20.833,21.232],["binary,",21.232,22.105],["down",22.105,22.367],["the",22.367,22.539],["pub",22.539,22.823],["tonight!",22.823,23.756]]],["verse1",[["Me",26.994,27.27],["agent's",27.27,27.907],["a",27.907,27.967],["grafter,",27.967,28.711],["it",28.711,28.874],["codes",28.874,29.343],["all",29.343,29.711],["night,",29.711,30.007]]],["verse1",[["But",30.311,30.441],["it",30.441,30.605],["guesses",30.605,31.304],["a",31.304,31.364],["lot",31.364,31.723],["and",31.888,32.012],["it",32.012,32.202],["never",32.202,32.596],["gets",32.596,32.895],["it",32.895,33.04],["right,",33.04,33.454]]],["verse1",[["So",33.54,33.968],["Tov",33.968,34.387],["tells",34.387,34.672],["it",34.672,34.891],["straight",34.891,35.24],["what",35.24,35.415],["it",35.415,35.604],["needs",35.604,36.372],["to",36.372,36.537],["do,",36.537,36.851]]],["verse1",[["Here's",36.956,37.24],["the",37.24,37.393],["error,",37.393,37.799],["here's",37.799,38.078],["the",38.078,38.258],["fix,",38.258,38.662],["and",38.662,38.941],["the",38.941,39.106],["build",39.106,39.52],["goes",39.52,39.939],["through!",39.939,40.393]]],["chorus",[["So",40.393,40.553],["it's",40.553,40.777],["Tov!",40.777,41.61],["Tov!",41.61,42.309],["Faster",42.309,42.957],["than",42.957,43.334],["Rust!",43.334,43.815]]],["chorus",[["Written",44.015,44.494],["like",44.494,45.033],["TypeScript,",45.033,45.746],["it's",45.746,45.931],["Tov",45.931,46.36],["or",46.36,46.804],["bust!",46.804,47.223]]],["chorus",[["Your",47.412,47.715],["agent",47.715,48.395],["writes",48.395,48.759],["it",48.759,49.143],["and",49.143,49.303],["Tov",49.303,49.742],["puts",49.742,50.031],["it",50.031,50.156],["right,",50.156,50.465]]],["chorus",[["Ship",50.465,51.009],["one",51.009,51.416],["little",51.416,51.867],["binary,",51.867,52.71],["down",52.71,52.99],["the",52.99,53.154],["pub",53.154,53.429],["tonight!",53.429,53.932]]],["verse2",[["No",53.932,54.436],["any,",54.436,55.025],["no",55.025,55.299],["null,",55.299,55.898],["no",55.898,56.132],["double",56.132,56.497],["equals",56.497,56.985],["sign,",56.985,57.39]]],["verse2",[["Put",57.39,57.519],["a",57.519,57.714],["try",57.714,58.317],["on",58.317,58.517],["the",58.517,58.652],["call",58.652,58.997],["and",58.997,59.138],["it'll",59.138,59.438],["all",59.438,59.969],["be",59.969,60.388],["fine,",60.388,60.807]]],["verse2",[["Forty-two",60.807,61.665],["milliseconds",61.665,62.533],["and",62.533,62.738],["the",62.738,62.942],["build",62.942,63.356],["is",63.356,63.785],["done,",63.785,64.139]]],["verse2",[["Poor",64.184,64.459],["old",64.459,64.618],["Rust's",64.618,65.062],["still",65.062,65.312],["compiling,",65.312,66.135],["it's",66.135,66.344],["missed",66.344,66.594],["all",66.594,67.003],["the",67.003,67.172],["fun!",67.172,67.926]]],["bridge",[["Tap,",67.97,68.449],["tap,",68.449,68.859],["tap",68.859,69.302],["go",69.302,69.572],["the",69.572,69.82],["agent's",69.82,70.55],["keys,",70.55,71.338]]],["bridge",[["Bleep,",71.418,71.847],["bleep,",71.847,72.276],["bleep,",72.276,72.63],["check",72.63,72.974],["the",72.974,73.104],["JSON",73.104,73.967],["please,",73.967,74.73]]],["bridge",[["Five!",74.8,75.663],["Four!",75.663,76.481],["Three!",76.481,76.905],["Two!",76.905,77.402],["One!",77.402,78.222]]],["bridge",[["(DING!)",78.222,79.694],["That's",79.694,80.133],["the",80.133,80.302],["build",80.302,80.722],["done!",80.722,81.176]]],["final",[["So",81.18,81.362],["it's",81.362,81.585],["Tov!",81.585,82.453],["Tov!",82.453,83.166],["Faster",83.166,83.703],["than",83.703,84.103],["Rust!",84.103,84.817]]],["final",[["Written",84.817,85.436],["like",85.436,85.815],["TypeScript,",85.815,86.528],["it's",86.528,86.708],["Tov",86.708,87.142],["or",87.142,87.566],["bust!",87.566,88.205]]],["final",[["Your",88.205,88.504],["agent",88.504,89.204],["writes",89.204,89.584],["it",89.584,89.911],["and",89.911,90.08],["Tov",90.08,90.509],["puts",90.509,90.804],["it",90.804,90.945],["right,",90.945,91.387]]],["final",[["Ship",91.387,91.781],["one",91.781,92.185],["little",92.185,92.634],["binary,",92.634,93.512],["down",93.512,93.767],["the",93.767,93.927],["pub",93.927,94.206],["tonight!",94.206,95.124]]],["outro",[["Tov!",95.125,96.002],["Tov!",96.002,96.73],["Faster",96.73,97.329],["than",97.329,97.666],["Rust!",97.666,98.536]]],["outro",[["Tov!",98.536,99.389],["Tov!",99.389,100.232],["Tov",100.232,100.716],["or",100.716,101.145],["bust!",101.145,101.953]]],["outro",[["Curl",101.953,102.327],["it,",102.327,102.821],["pipe",102.821,103.106],["it,",103.106,103.639],["off",103.639,104.019],["you",104.019,104.522],["go,",104.522,105.316]]],["outro",[["Tov",105.316,106.164],["dot",106.164,107.022],["S-H...",107.022,107.948],["OI!",107.948,108.868]]]];
// The song's loudness, 30 times a second: its bass, overall and treble, and the voice alone (from
// the separated vocals), one byte each (base64).
const ENV = "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W3nJU2jLwnr//3KN5edQjJq3SZ6Ooj6AXJExfmqBJlJmcWFLbJHIaVqJ621fi/RsVXamTFtoTCap3bhz//9tm8fdWqWTl0NigIc4P091LjVhaDAgXHFtVViiYLJUgqBsTnjkYlZvpEg6ZKdBpczudOzwcK2ty12uhrdSwWi3Ss1wrEa7YKpAw1enScdPq3O5SK5SyEOaQbtGhS6dQo53d+b2kL7i5m2koLlUt4m3TsJmqU6zY7FMwVibQrRKp2q6Tqd3sU+VW5hPijuBR2onWFa0imX//22k1ONMmIejSZOBlY99UIXCjFuEwohGcrVqRHq8V0x/iUBSfGIuTIBEJkxvRBmNxq8Q//95DeniZAuenlELhJFKC15xTQ5agztiTq1NxzKoarg2rWXCO6RWuDifS7wonVy+2vmasfb6ZbfE2FumjLJSqmehTahelEybWI9Lm06ZVKJIpW+hSZNflDuCTIBBaF5PRJHQa/v5rJf3/GOqstZRxpXRR+Jsr0+5X5pKkFGVNZ9FhmCIQ6BxqDmqYsA/oEq+RpdDsE++iav//3et4fpYqZWnTmqJokV4aIRQT3CAp1lpe8ZgaY7eeWCwn71SqFa9TbhB30eCQoiQwLNo//t7RO/iXjuVl1MyipRPNFh0QzJmcjkqZqFGkVq3erpVmV2IUn41VlJvVTYqb+lmxOe/rfrzXaO21lCnhbpUxG68SNJVwz/3SL429E2RRKtCmm+mO4NffUR1QF1CZTdKNZB4fdPxlq3n9m6rrbJadKCXTEJqibJZW4HgbEV022tTgMtqQKB7nUGqVKk4iTt7OHU7a0a2oaX//o+j4PNliqzCUKWPskSWYYxFe118MVpkcyw+Y4FuOVmNxFRbid1gV4XnaFd86Gt/1t2m//2EqODyW6GMsVCZg5hFa1p9PF5xeDJHYHsuQmKAZC1igFQxVHc8M1p4MThqhjpzq92DsP/+XajF11urh6RWnXKMRXhjgz5uaos6e2CKP3pPi2lyR5tZkkqaTZVHk0ebMZd3nNPnland42CepqxThpe4TbZip0m4UrA/0FacQLlJh1ySSZ9xqkh4S1lIbzNNUGQwTFC5h6bj5nyi0eZckZayTJeMrUChWaE9s1yeNKxPhi+JRYhQiTu5YNoxn061LalB0SuPPqlsyZKg7/J0t8fVZnuAwFjFfKdSnFafRaVmoDqo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SIFRiEKNcow3o4yFN612mD+oZqU2pGezWuumzL7rmr60zYSyqMqBtYi25alEkP+aS4r/kViN9Xxvj99papiqUneVnk93jr1ZWH1tLpfq1yTu6JcqvrqGOubHbj+PoV1KV4tQS3CRQFVahUYxZZWDJVyQaBtlilEiXZDgY0S2zoCx/6XGsOGIzKHOfNaJyX3SU9d6/0HJa/5N2WD/VNZw/1LFh+xWnnOdW6lcm2mQYn1NvaJb5/+oSeLVhlfAunJbtr9oWmWRXmZqhkJQaJA6NHmZfSp8n4oadJJcF3aPfDR1l/91a9/MfdL4j8Ge24LQi+R6/3fZb/xAumPnVuxa/0fCWPlS03z/XLBzwV2cWplflGKHPIp1dK7/x1nY45Zgv8B9fdHLbWZ6mmlqPIZWbGOLPGZnlV9VXZCdMkmObxlaiVYYXo3+Yjmx/2Sf/7Wns+N92YrJdt2j22/cWsBs3kDmZf9Iw2HuVK9/wk3OmPdR03X1Tbda00iDWIU+wp2kqOqg257ej+6GyGvmbsFg4T28X+9A2F3/P6ZXzEanfLJRwXzEXKdfpmmPSFdbclE7c+C/da/zid2l6H/5t9F2tmSZuYo+hf+HRo//jT6N9X82nelmO5zBOkyQeh5SjnkmQLamnaL9saLI74y4ntB2ybThbtpbx2vnNb1g6UadTKpBkWKBQqaDTz6RayVFf0keRXyUUj+w0HWr6qS4tdB8v6DadOih2HHWUMBt6TvIY/9JrWbEXKKYlE6yqbY/xoPmUNRj+E2vY8Rs1buN4+WcW9DIhWjTxG17n6VgZF2KV2htk0VmYZhEaGychTtwnYUqb5VXKmySVyRRr41Vjv63x8nqkre96HvZvdZrqnKxc5dHjlx7ZItCRmWUYktntpCpbcR9z2zFasFyxGPWRLh/sc7+pHPk24VwtKxwfMDFantnkWh8XpVOg2iVQ3tci21Tap6MKnCgYxxzlEQie65Jmkz0sffB+67Rs/qF/577f/+V8Hj/RpBsqU9/dXdLl3eXSZCfhkWLr1xJgZJFRId6YT+BmXZv5/OYyu+gqbzVjMm42H/Qdsl2y0bActlOwGPhSq5rwEO6oLxCnIOPUHtMR0uBOkk6iYxYkve9i8bpgsOq33jtxOxy8WfYa/9G4WH/YclU9kjQZP9L35H/VuBx+FXbW/9g1FrnN9KU2sDvr5fL04yIrbRwi6KzcHlUgV1rXIhIbGCWPFpukXJHeJyGMneYYiJ9mpZGeJX/ckzWwoui76Djm9eH7Ynvhf929Xb/P8Vq/yzpX/8brlPwDpVHzwyIN70PiSi+EGkokAlpRoVx1ciDyNyeorW5eZW5tGuSfJdmkkKHTqY+hzyqF2cvjA5YKHgMRyBhCjkcTQhHVGETltSWi/+87qX/lf+E5oj/ltV+7kjBev4rwW7/JJdhzQ94Q6YNbFyVCXPcoAt5p6kLXk6DM7ubdMf/rpXi4oGir65wlJqxZ49XiFiUPYxErSVkMYEPVSh0CUUiXwpCHloKSidnCIRrtVv4wu/C/6fzpN2L7p3Bh891vZHXNJruyDGT/8AXjf/AC4f/uwuE/7cLd/+kCnb8pAyos+CB/8P/r+6c66Xgiv+p6ID/ZNR1/zambtwlim24FXpQpwd6wagIi//BCYj/vAWB/7MbvLrSmP+0xbbgltyNvHvShdVv/E6/bfw2zWj/LspY/xXUdP8q15j/MMyB/0rFZf4psWjsPtOs8aXipsOr1YfSisR+2IfAbs5Dq2zSQ5NeqByBS5Eqh5xkRbmYPzHLeEY+yF1FO75rNGfusCbi8osf1tB4G9jXYxiYu2ATT7FjD0XASxMij1EKF4OPBR6Ucwgcm0wJJXs6ChZ8aQim/8oI1OSMCLW7agzHt00OeItYCT2cVAU6kj8GHHNcBBCZoQIYqXQCGZ9MAhR9OQIxp4cB0/+vAM7niAG1wm4Cx8RfA16DYwE9g1EDM3o6Aw91iQEPtpQBEsRlAxK3UAMMhEACUcW4AdP8qwDO3nkBv8loAazMZAFPoFgBQadGAR+YSQEamoYBGqaIABGgWQATjz4ACm1MAIr9xADS8ZEBxcVzBMfHXgl4jGQFO5NcBDONPwQab1EBDZWOARSybgEVi0wCDXs9AhafdAGd/7oB5fKIArrEdAPNy18EZo5lBEOTWQM0hEAEEWttBAmWogMNk2gHDoZCCA14Nwc5tawGzf26BdbhjQi4wG8KubZkCVepdgo4r1cLJ5A/CA91fAURkpMFD45gBRCEOwUKbzgFXseyBtnolwvi1nYO2dNmG46sXyFLeFYbQ3I9Gy1oQiIlc5EZQJmaDDygXQ1Zlj4MRXlZC5D7ugjl8poHz9RxB+bbXQeKpF8IUZxfCUKSPwkZcVUJFoeSCBWgbQoZiEoMGIA5DiWNhgvB/8MI2uaVCcjTbwjCxF0JYo5qBkGQUwYrfjsIEVpuBxiJogQQpW8GFphJCBRzNgw/wLAJ0/21CtPlggvQz2sNsLdcEE6AbRNHelUSImBFFRFmfRMOl5MODKBoDxWkUw8LfEcLeOS3Bun0rgbDyoIH1sloCpeeZwpAmnIHQq5YChlvTQwLdKUJEKuTEw65bBERp0gNEn5lCqX/yAjZ96APx8+BF+PRYxtxiG4aNYNnEx+VUw8RYGERD5SEDROqbhMbpVYSG3pGFjSlkBPT/74Q8+eOEtS7bxnWxmInX6J4J0a+WCAplD4XF3RzFROMkBQQhWcVE3hFFgpYOhNq4sUN4v+0C9fgjBLXzG8Ysr1qIlF1YBxLckgZJ2BBFxhykxsvlq8bSJFZHU2JPSdFakoq///AKf//oDH//Hgy/+NbOrSoRD1jdD44SFk2MjY/Ky4bNyY0DzAcMhMmFyMaJBMcDx4NHAQaCh4DFggbAhIGFwERBRYCEwUZARQFGQ==";
const BPM = 141, BEAT = 60 / BPM, BEAT0 = 0.346;
const SONG = "/fast-with-tov.mp3";
const INSTALL = "curl -fsSL https://tov.sh/install.sh | sh";

const C = {
  amber: "#F2B35B", amberHi: "#FFD08A", foam: "#FFF4E2", ink: "#1A1424", rust: "#E8714A",
  green: "#8FE3BE", red: "#FF8A7A", cyan: "#9DD3FF", pink: "#F59AB8", violet: "#B9A4FF",
  lime: "#C9E99A", yellow: "#FFD98A", blue: "#7C9CFF", orange: "#F49B5C",
};
const FONT = `"Schibsted Grotesk", ui-sans-serif, system-ui, sans-serif`;
const MONO = `ui-monospace, "SF Mono", Menlo, Consolas, monospace`;
const calm = matchMedia("(prefers-reduced-motion: reduce)").matches;

// ---------------------------------------------------------------- small maths

const clamp = (v, a, b) => (v < a ? a : v > b ? b : v);
const lerp = (a, b, k) => a + (b - a) * k;
const out3 = k => 1 - Math.pow(1 - clamp(k, 0, 1), 3);
const inOut = k => (k = clamp(k, 0, 1), k < 0.5 ? 4 * k * k * k : 1 - Math.pow(-2 * k + 2, 3) / 2);
// (a small overshoot: things arrive, and settle, without bouncing)
const back = k => { k = clamp(k, 0, 1) - 1; return 1 + k * k * (1.9 * k + 0.9); };
const TAU = Math.PI * 2;
function hash(n) { n = Math.sin(n * 127.1 + 311.7) * 43758.5453; return n - Math.floor(n); }
function hex(h) { const n = parseInt(h.slice(1), 16); return [n >> 16, (n >> 8) & 255, n & 255]; }
function mix(a, b, k) { return `rgb(${Math.round(lerp(a[0], b[0], k))},${Math.round(lerp(a[1], b[1], k))},${Math.round(lerp(a[2], b[2], k))})`; }

const envBytes = Uint8Array.from(atob(ENV), ch => ch.charCodeAt(0));
function env(t, k) {
  const f = t * 30, i = Math.floor(f), n = envBytes.length / 4 - 1;
  if (i < 0 || i >= n) return 0;
  const a = envBytes[i * 4 + k], b = envBytes[i * 4 + 4 + k];
  return (a + (b - a) * (f - i)) / 255;
}

// ---------------------------------------------------------------- the song as data

const lines = LYRICS.map(([sec, ws], i) => {
  const words = ws.map(([text, s, e], j) => ({ text, s, e, j, el: null, lit: false }));
  return { i, sec, words, s: words[0].s, e: words[words.length - 1].e, el: null };
});
// A line takes the stage a little before its first word (once the line before has mostly sung
// its last), and leaves when the next one comes, or just after its last word.
lines.forEach((l, i) => {
  const before = lines[i - 1];
  if (!before) { l.show = -1; return; }
  const last = before.words[before.words.length - 1];
  const after = last.s + 0.6 * (last.e - last.s);
  l.show = Math.max(after, Math.min(l.s - 0.6, l.s - 0.05));
  if (l.show > l.s - 0.05) l.show = Math.max(last.s + 0.1, l.s - 0.05);
});
lines.forEach((l, i) => { l.hide = i + 1 < lines.length ? Math.min(lines[i + 1].show, l.e + 0.35) : l.e + 2; });
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
  intro: { sky: ["#140E26", "#33184A", "#6B2A5E"], tag: C.amber, bg: "pub" },
  chorus: { sky: ["#22101C", "#6A2230", "#C8603A"], tag: C.amber, bg: "burst" },
  verse1: { sky: ["#090C22", "#17204E", "#2C3A82"], tag: C.cyan, bg: "night" },
  verse2: { sky: ["#071A1C", "#0E3E3D", "#1C6B62"], tag: C.green, bg: "grid" },
  bridge: { sky: ["#0C0920", "#25124F", "#5A2878"], tag: C.violet, bg: "arcade" },
  final: { sky: ["#240C12", "#7A2328", "#D47634"], tag: C.amber, bg: "burst" },
  outro: { sky: ["#12112E", "#36205F", "#97405E"], tag: C.pink, bg: "burst" },
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
#ts { position: fixed; inset: 0; z-index: 1000; background: #140E1E; color: #fff; font-family: ${FONT}; overflow: hidden; touch-action: manipulation; -webkit-user-select: none; user-select: none; -webkit-tap-highlight-color: transparent; contain: strict; }
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
#ts .ts-name { display: flex; align-items: center; gap: 10px; font-weight: 700; letter-spacing: -.02em; font-size: 1.0625rem; line-height: 1.1; }
#ts .ts-name small { display: block; font-weight: 500; font-size: .8125rem; letter-spacing: 0; opacity: .6; }
#ts .ts-name svg { width: 42px; height: auto; }
#ts .ts-x { width: 44px; height: 44px; border-radius: 50%; background: rgba(255,255,255,.1) !important; color: #fff; display: grid; place-items: center; transition: background .2s; }
#ts .ts-x:hover { background: rgba(255,255,255,.18) !important; }
#ts .ts-x svg { width: 18px; height: 18px; }
/* where we are in the song */
#ts .ts-sec { position: absolute; top: max(80px, calc(env(safe-area-inset-top) + 66px)); left: 50%; z-index: 2; display: flex; align-items: center; gap: 8px; font-size: .9375rem; font-weight: 600; padding: 7px 14px 7px 12px; border-radius: 99px; color: rgba(255,255,255,.88); background: rgba(10,6,20,.3); border: 1px solid rgba(255,255,255,.12); white-space: nowrap; transform: translateX(-50%); transition: opacity .3s; }
#ts .ts-sec::before { content: ""; width: 7px; height: 7px; border-radius: 50%; background: var(--tag, ${C.amber}); }
#ts .ts-sec.swap { animation: ts-in .6s cubic-bezier(.2,.8,.2,1); }
#ts .ts-hint { position: absolute; left: 50%; top: max(128px, calc(env(safe-area-inset-top) + 114px)); transform: translateX(-50%); z-index: 2; font-size: .875rem; font-weight: 500; padding: 8px 14px; border-radius: 99px; color: rgba(255,255,255,.75); background: rgba(10,6,20,.3); white-space: nowrap; opacity: 0; transition: opacity .6s; pointer-events: none; }
#ts .ts-hint.show { opacity: 1; }
#ts .ts-hint b { color: ${C.amber}; font-weight: 700; }
/* the lyrics: the line being sung, the next one under it */
#ts .ts-lyrics { position: absolute; left: max(14px, env(safe-area-inset-left)); right: max(14px, env(safe-area-inset-right)); bottom: calc(92px + env(safe-area-inset-bottom)); z-index: 2; pointer-events: none; height: 0; }
#ts .ts-l { position: absolute; left: 0; right: 0; bottom: 0; margin: 0 auto; max-width: 18em; text-align: center; font-weight: 800; font-size: clamp(1.5rem, min(5vw, 7.5vh), 6rem); line-height: 1.12; letter-spacing: -.03em; text-wrap: balance; transform-origin: 50% 100%; opacity: 0; transform: translateY(.5em) scale(.5); visibility: hidden; transition: transform .7s cubic-bezier(.2,.8,.2,1), opacity .5s ease, visibility 0s .7s; -webkit-font-smoothing: antialiased; }
#ts .ts-l.is-cur { opacity: 1; visibility: visible; transform: translateY(calc(var(--nh, 0px) * -1 - .35em)) scale(1); transition-delay: 0s; }
#ts .ts-l.is-next { opacity: .3; visibility: visible; transform: scale(.42); transition-delay: 0s; }
/* (a sung line clears out quickly, up and away) */
#ts .ts-l.is-prev { opacity: 0; visibility: visible; transform: translateY(calc(var(--nh, 0px) * -1 - var(--h, 1em) - .3em)) scale(.85); transition: transform .35s cubic-bezier(.4,0,.6,1), opacity .2s ease, visibility 0s .35s; }
/* a word: dim until it's sung, then filled left to right in time with the voice, through a soft
   edge (the fill is the text's own background, so nothing sits on top of it to be cut off) */
#ts .ts-in { display: block; transform-origin: 50% 100%; }
#ts .ts-w, #ts .ts-c { position: relative; display: inline-block; white-space: pre; transform-origin: 50% 85%; color: transparent; -webkit-background-clip: text; background-clip: text; background-image: linear-gradient(90deg, var(--c, #fff) calc(var(--p, 0) * (100% + .4em) - .4em), rgba(255,255,255,.34) calc(var(--p, 0) * (100% + .4em))); }
/* (a word that moves letter by letter: its letters carry the fill) */
#ts .ts-w.ts-letters { background-image: none; }
/* a few words have a colour of their own */
#ts .ts-w.ts-tov { --c: ${C.amber}; }
#ts .ts-w.ts-rust { --c: ${C.rust}; }
#ts .ts-w.ts-code { font-family: ${MONO}; font-weight: 700; letter-spacing: -.05em; font-size: .86em; --c: ${C.cyan}; }
#ts .ts-w.ts-good { --c: ${C.green}; }
#ts .ts-w.ts-bad { --c: ${C.red}; }
#ts .ts-w.ts-no::after { content: ""; position: absolute; left: -4%; right: -4%; top: 54%; height: .07em; border-radius: .04em; background: ${C.red}; transform: scaleX(0); transform-origin: 0 50%; transition: transform .35s cubic-bezier(.2,.8,.2,1); }
#ts .ts-w.ts-no.is-done::after { transform: scaleX(1); }
#ts .ts-w.ts-big, #ts .ts-w.ts-tov { margin: 0 .05em; }
@keyframes ts-in { 0% { opacity: 0; transform: translate(-50%, -6px); } 100% { opacity: 1; transform: translateX(-50%); } }
/* the controls */
#ts .ts-bar { position: absolute; left: 0; right: 0; bottom: 0; z-index: 3; display: flex; align-items: center; gap: 14px; padding: 14px max(20px, env(safe-area-inset-right)) max(18px, env(safe-area-inset-bottom)) max(20px, env(safe-area-inset-left)); }
#ts .ts-play { width: 48px; height: 48px; border-radius: 50%; background: ${C.foam} !important; color: ${C.ink} !important; display: grid; place-items: center; flex: none; transition: transform .15s; }
#ts .ts-play:hover { transform: scale(1.05); }
#ts .ts-play:active { transform: scale(.94); }
#ts .ts-play svg { width: 20px; height: 20px; }
#ts .ts-track { position: relative; flex: 1; height: 28px; cursor: pointer; touch-action: none; }
#ts .ts-rail { position: absolute; left: 0; right: 0; top: 50%; height: 4px; margin-top: -2px; border-radius: 2px; background: rgba(255,255,255,.18); overflow: hidden; }
#ts .ts-fill { position: absolute; inset: 0; background: ${C.foam}; transform-origin: 0 50%; transform: scaleX(0); }
#ts .ts-tick { position: absolute; top: 50%; width: 2px; height: 10px; margin: -5px 0 0 -1px; border-radius: 1px; background: rgba(255,255,255,.35); }
#ts .ts-knob { position: absolute; top: 50%; width: 14px; height: 14px; margin: -7px 0 0 -7px; border-radius: 50%; background: #fff; }
#ts .ts-time { font-size: .8125rem; font-weight: 600; font-variant-numeric: tabular-nums; min-width: 5.6em; text-align: right; color: rgba(255,255,255,.7); }
/* the cards at the start and the end */
#ts .ts-card { position: absolute; inset: 0; z-index: 4; display: grid; place-items: center; padding: 24px 16px; overflow-y: auto; background: radial-gradient(ellipse at 50% 30%, #4A1F4E, #140E26 75%); text-align: center; transition: opacity .45s ease, transform .45s ease; }
#ts .ts-card[hidden] { display: none; }
#ts .ts-card.away { opacity: 0; transform: scale(1.08); pointer-events: none; }
#ts .ts-card h2 { font-size: clamp(2.5rem, min(9vw, 11vh), 5.5rem); line-height: .95; letter-spacing: -.045em; font-weight: 800; margin: 16px 0 12px; color: #fff; }
#ts .ts-card h2 em { font-style: normal; color: ${C.amber}; }
#ts .ts-card p { margin: 0 auto 28px; max-width: 28rem; color: rgba(255,255,255,.7); font-size: 1.0625rem; line-height: 1.55; }
#ts .ts-card p b { color: #fff; font-weight: 600; }
#ts .ts-go { display: inline-flex; align-items: center; gap: 10px; padding: 15px 28px; border-radius: 99px; background: ${C.amber} !important; color: ${C.ink} !important; font-weight: 700 !important; font-size: 1.125rem !important; letter-spacing: -.01em; box-shadow: 0 10px 40px rgba(242,179,91,.25); transition: transform .2s cubic-bezier(.2,.8,.2,1), background .2s; }
#ts .ts-go:hover { transform: translateY(-1px); background: ${C.amberHi} !important; }
#ts .ts-go:active { transform: scale(.98); }
#ts .ts-go svg { width: 20px; height: 20px; }
#ts .ts-keys { margin-top: 24px; font-size: .875rem; color: rgba(255,255,255,.7); }
#ts .ts-keys kbd { font: inherit; font-weight: 600; padding: 2px 7px; border-radius: 6px; background: rgba(255,255,255,.1); color: rgba(255,255,255,.85); }
#ts .ts-row { display: flex; flex-wrap: wrap; gap: 10px; justify-content: center; }
#ts .ts-ghost { padding: 14px 22px; border-radius: 99px; background: rgba(255,255,255,.1) !important; font-weight: 600; }
#ts .ts-ghost:hover { background: rgba(255,255,255,.16) !important; }
#ts .ts-birdy { width: clamp(80px, 15vh, 120px); height: auto; animation: ts-hover 2.4s ease-in-out infinite; }
#ts .ts-birdy .hb-wing { transform-box: fill-box; transform-origin: 92% 96%; animation: ts-flap .07s ease-in-out infinite alternate; }
#ts :focus-visible { outline: 2px solid ${C.amber}; outline-offset: 3px; }
@keyframes ts-hover { 0%, 100% { transform: translateY(0) rotate(-3deg); } 50% { transform: translateY(-8px) rotate(-1deg); } }
@keyframes ts-flap { from { transform: rotate(-12deg); } to { transform: rotate(30deg) scale(.92, .55); } }
/* small screens, and short ones (a phone on its side) */
@media (max-width: 640px) {
  #ts .ts-name small { display: none; }
  #ts .ts-name svg { width: 36px; }
  #ts .ts-sec { font-size: .875rem; top: max(70px, calc(env(safe-area-inset-top) + 58px)); }
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
  #ts .ts-sec { top: 12px; font-size: .8125rem; padding: 5px 12px 5px 10px; }
  #ts .ts-hint { display: none; }
  #ts .ts-lyrics { bottom: calc(62px + env(safe-area-inset-bottom)); }
  #ts .ts-l { font-size: clamp(1.25rem, min(4.2vw, 8vh), 2.6rem); }
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
  #ts .ts-w { transition: none; }
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
  "ts-big": ["oi", "agents", "bust", "ding", "five", "four", "three", "two", "one"],
  "ts-rust": ["rust", "rusts"],
  "ts-code": ["typescript", "json", "try", "curl", "pipe", "sh", "binary"],
  "ts-good": ["fix", "fine", "through", "done"],
  "ts-bad": ["error", "never"],
};
const VOICE = new Map();
for (const [cls, ws] of Object.entries(VOICES)) for (const w of ws) if (!VOICE.has(w)) VOICE.set(w, cls);
function wordClass(text, before) {
  const w = text.toLowerCase().replace(/[^a-z0-9]/g, "");
  if (w === "any" || w === "null" || w === "equals") return "ts-code ts-no";
  // (an agent that never gets it right isn't right)
  if (w === "right") return before.includes("never") ? "ts-bad" : "ts-good";
  const cls = VOICE.get(w) || "";
  // (shouts and the countdown are the words sung with a "!"; "agent's", "one little" aren't)
  if (cls === "ts-big" && !text.includes("!")) return "";
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
  const lyricHtml = lines.map(l => `<p class="ts-l"><span class="ts-in">${l.words.map((w, j) => {
    const shown = w.text.replace(/^\((.*)\)$/, "$1");
    const before = l.words.slice(0, j).map(x => x.text.toLowerCase());
    w.motion = motionOf(shown, w.e - w.s);
    const byLetter = w.motion === "wave" || w.motion === "type";
    const inner = byLetter ? [...shown].map(ch => `<span class="ts-c">${ch}</span>`).join("") : shown;
    return `<span class="ts-w ${wordClass(shown, before)}${byLetter ? " ts-letters" : ""}"${byLetter ? ` aria-label="${shown}"` : ""}>${inner}</span>`;
  }).join(" ")}</span></p>`).join("");
  root.innerHTML = `
<div class="ts-sky" aria-hidden="true"></div><div class="ts-sky" aria-hidden="true"></div>
<canvas class="ts-cv" aria-hidden="true"></canvas>
<div class="ts-scan" aria-hidden="true"></div><div class="ts-flash" aria-hidden="true"></div>
<div class="ts-top">
  <div class="ts-name">${svgOf(`${BIRD_TAIL}${BIRD_BODY}${BIRD_WING}${BIRD_HEAD}`)}<span>Fast with Tov<small>a singalong</small></span></div>
  <button type="button" class="ts-x" aria-label="Close the singalong">${ICON.close}</button>
</div>
<div class="ts-sec" aria-hidden="true"></div>
<div class="ts-hint">Tap along on every <b>Tov</b></div>
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
    <p>A singalong for coding agents and the people who buy them pints. Follow the words, and <b>tap along on every Tov.</b></p>
    <button type="button" class="ts-go">${ICON.play}Pints up!</button>
    <div class="ts-keys"><kbd>Space</kbd> pause · <kbd>←</kbd> <kbd>→</kbd> skip · <kbd>Esc</kbd> back to the page</div>
  </div>
</div>
<div class="ts-card ts-end" hidden>
  <div>
    ${BIRD_SVG}
    <h2>That's the <em>build</em> done.</h2>
    <p>Now the real thing:</p>
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
    lines[i].inner = el.firstElementChild;
    el.querySelectorAll(".ts-w").forEach((w, j) => {
      const word = lines[i].words[j];
      word.el = w;
      word.letters = word.motion === "wave" || word.motion === "type" ? [...w.children] : null;
    });
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
  ctx.font = `${o.weight || 800} ${size}px ${o.font || FONT}`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  if (o.outline) {
    ctx.lineJoin = "round";
    ctx.lineWidth = size * o.outline;
    ctx.strokeStyle = C.ink;
    ctx.strokeText(text, 0, 0);
  }
  ctx.fillStyle = col;
  ctx.fillText(text, 0, 0);
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
const CONF = [C.amber, C.foam, C.pink, C.cyan, C.green, C.violet];
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
      bigText(p.text, p.x, p.y, p.s, p.col, { a, rot: p.rot, scale: sc });
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
function shake(n) { if (!calm) shakeAmt = Math.max(shakeAmt, n * 0.45); }
function flash(a, col = "#fff") { if (!calm) { flashAmt = Math.max(flashAmt, a); els.flash.style.background = col; } }
function punch(n) { if (!calm) zoomAmt = Math.max(zoomAmt, n); }

// a word slammed onto the stage: it lands from a little bigger, settles, and lifts away
function stamp(t, text, o = {}) {
  const life = o.life ?? 0.8;
  const col = o.col || C.foam;
  const at = () => [cx() + (o.dx ?? 0) * U * narrow(), (o.y ? Ht * o.y : cy() - 40 * U) + (o.dy ?? 0) * U];
  scene(t, t + life, now => {
    const dt = now - t;
    const k = out3(dt / 0.16);
    const leave = clamp((dt - (life - 0.25)) / 0.25, 0, 1);
    const sc = lerp(1.35, 1, k) * (1 + leave * 0.08);
    const a = Math.min(clamp(dt / 0.06, 0, 1), 1 - leave);
    const [x, y] = at();
    const size = (o.size ?? 200) * U;
    drawGlow(o.glow || "amber", x, y, size * 1.6, 0.35 * a);
    bigText(text, x, y, size, col, { a, rot: (o.rot ?? 0) * 0.6, scale: sc });
  }, 0.001);
  on(t, () => {
    const [x, y] = at();
    shake(o.shake ?? 12);
    punch(0.02);
    ring(x, y, col, 240, 0.6, 3);
    sparks(Math.round((o.sparks ?? 18) * 0.6), x, y, col);
  });
}

function fadeIn(now, s, d = 0.35) { return clamp((now - s) / d, 0, 1); }

// ---- the intro: Oi! Agents! Pints up!
function intro(li) {
  const oi = W(li, 0), agents = W(li, 1), pints = W(li, 2), up = W(li, 3);
  stamp(oi.s, "OI!", { size: 260, rot: -0.12, shake: 22, sparks: 30, col: C.amber });
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
  stamp(W(b, 2).s, "TOV!", { rot: -0.1, dx: -150, col: C.amber });
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
      ctx.fillStyle = ["#163452", "#1B3F63", "#204A74"][r];
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
      for (let i = 0; i < 3; i++) bigText("z", mx - 70 * U - i * 26 * U, my + 20 * U - ((n * 40 + i * 30) % 90) * U, (20 + i * 6) * U, C.foam, { a: 0.6 * clamp(1 - ((n * 40 + i * 30) % 90) / 90, 0, 1), weight: 800 });
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
        bigText("?", qx, qy, (40 + hash(i) * 30) * U, C.cyan, { a: 1 - k, rot: (hash(i * 2) - 0.5) * 0.6, scale: back(k * 5) });
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
    bigText(`${Math.round(42 * out3(k))}`, x, y - 8 * U, 84 * U, C.amber, { a });
    mono("ms", x, y + 46 * U, 22 * U, C.foam, a, "center");
    if (now > done.s) {
      const k2 = back((now - done.s) / 0.3);
      bigText("BUILD DONE", x, y + r + 50 * U, 44 * U, C.green, { a, scale: k2, rot: -0.04 });
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
  }, 0.25);
  // the party it missed, going off somewhere else
  on(fun.s, () => confetti(30, cx() + 300 * U * narrow(), cy() - 120 * U, 0.7));
  on(comp.s, () => floatText("still compiling…", cx() - 170 * U, cy() - 80 * U, "#FF8A5C", 26, 1.6));
}

// ---- the bridge: tap tap tap, bleep bleep bleep, 5 4 3 2 1, DING
const KEYROWS = ["QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"];
const keyLit = new Map();
function lightKey(ch, t) { keyLit.set(ch.toUpperCase(), t); }
function bridge(b) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  // the keyboard
  scene(l0.s - 0.1, until(l1.i) - 0.3, now => {
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
  scene(json.s - 0.1, until(l1.i) - 0.3, now => {
    const a = fadeIn(now, json.s - 0.1, 0.2);
    const fs = Math.max(14, 30 * U);
    const y = cy() - 160 * U;
    mono(`{ "build": "ok" }`, cx(), y, fs, C.foam, a, "center");
    if (now > please.s) check(cx() + 190 * U, y, 40 * U, C.green, clamp((now - please.s) / 0.3, 0, 1));
  }, 0.2);
  // five, four, three, two, one
  const cols = [C.foam, C.foam, C.foam, C.foam, C.amber];
  for (let i = 0; i < 5; i++) {
    const w = W(b + 2, i);
    stamp(w.s, ["5", "4", "3", "2", "1"][i], { size: 280 + i * 30, col: cols[i], glow: "amber", rot: (i % 2 ? 0.06 : -0.06), life: 0.55, shake: 8 + i * 4 });
    target(w.s, "!");
    on(w.s, () => flash(0.03 + i * 0.015, cols[i]));
  }
  // DING!
  const ding = W(b + 3, 0), done = W(b + 3, 4);
  target(ding.s, "DING");
  on(ding.s, () => { flash(0.22, C.foam); shake(24); confetti(90, cx(), cy(), 1.6); ring(cx(), cy(), "#fff", 500, 0.9, 16); sparks(40, cx(), cy(), C.amberHi, 1.5); });
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
    bigText("DING!", x, y + 120 * U, 110 * U, "#fff", { scale: back(k / 0.2), rot: -0.06 });
    if (now > W(b + 3, 3).s) bigText("BUILD ✓", x, y + 210 * U, 46 * U, C.green, { scale: back((now - W(b + 3, 3).s) / 0.3) });
  }, 0.01);
  // the tension: the arcade gets brighter and faster until the DING
  bridgeSpan = [l0.s - 1, ding.s];
}
let bridgeSpan = [0, 0];

// ---- the outro: Tov! Tov! ... curl it, pipe it, off you go, Tov dot S-H, OI!
function outro(b) {
  const l0 = L(b), l1 = L(b + 1), l2 = L(b + 2), l3 = L(b + 3);
  stamp(W(b, 0).s, "TOV!", { rot: -0.1, dx: -150, col: C.amber });
  stamp(W(b, 1).s, "TOV!", { rot: 0.1, dx: 150 });
  target(W(b, 0).s, "TOV"); target(W(b, 1).s, "TOV");
  race(W(b, 2).s, l1.s + 0.2);
  stamp(W(b + 1, 0).s, "TOV!", { rot: -0.12, dx: -220, size: 170, col: C.amber });
  stamp(W(b + 1, 1).s, "TOV!", { rot: 0.0, dx: 0, size: 190 });
  stamp(W(b + 1, 2).s, "TOV!", { rot: 0.12, dx: 220, size: 210, col: C.amber });
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
      if (k > 0) bigText(p, x + widths[i] / 2, cy() - 30 * U, size, i === 0 ? C.amber : C.foam, { scale: back(k), a: k });
      x += widths[i];
    });
  }, 0.01);
  for (const at of [tov.s, dot.s, sh.s]) on(at, () => { shake(8); sparks(12, cx(), cy() - 30 * U, C.amber); });
  stamp(oi.s, "OI!", { size: 320, rot: -0.1, shake: 30, sparks: 40, life: 1.6, col: C.amber });
  target(oi.s, "OI!");
  on(oi.s, () => { flash(0.16, C.amber); confetti(110, cx(), cy(), 1.8); for (let i = 0; i < 6; i++) foam(10, Wd * (i + 0.5) / 6, Ht * 0.7); });
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
const BOKEH = Array.from({ length: 18 }, (_, i) => ({ x: hash(i * 3.1), y: hash(i * 7.7), r: 30 + hash(i * 1.9) * 90, sp: 0.02 + hash(i * 6.2) * 0.04, col: ["amber", "amber", "white", "pink", "amber"][i % 5] }));

function backdrop(name, a, t, pulse, tension) {
  if (a <= 0.01) return;
  ctx.save();
  ctx.globalAlpha = a;
  if (name === "pub" || name === "burst") {
    for (const b of BOKEH) {
      const x = ((b.x + t * b.sp) % 1.2 - 0.1) * Wd, y = (b.y * 0.8 + Math.sin(t * 0.5 + b.x * 9) * 0.03) * Ht;
      drawGlow(b.col, x, y, b.r * U * (1 + pulse * 0.15), a * (name === "pub" ? 0.2 : 0.12));
    }
    ctx.globalAlpha = a;
  }
  if (name === "burst") {
    ctx.save();
    ctx.translate(cx(), cy() - 30 * U);
    ctx.rotate(t * 0.12);
    ctx.fillStyle = `rgba(255,225,180,${0.035 + pulse * 0.04})`;
    const R = Math.hypot(Wd, Ht);
    for (let i = 0; i < 18; i++) {
      ctx.rotate(TAU / 18);
      ctx.beginPath(); ctx.moveTo(0, 0); ctx.lineTo(-R * 0.09, -R); ctx.lineTo(R * 0.09, -R); ctx.closePath(); ctx.fill();
    }
    ctx.restore();
    drawGlow("amber", cx(), cy() - 30 * U, 420 * U * (1 + pulse * 0.15), 0.18 * a);
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
    ctx.strokeStyle = `rgba(255,255,255,${0.05 + pulse * 0.05})`;
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
      sunGrad.addColorStop(0, C.amberHi);
      sunGrad.addColorStop(1, "#C8507A");
    }
    ctx.fillStyle = sunGrad;
    ctx.beginPath(); ctx.arc(cx(), hz, sr * (1 + pulse * 0.05), Math.PI, 0); ctx.fill();
    ctx.fillStyle = "#25124F";
    for (let i = 0; i < 6; i++) ctx.fillRect(cx() - sr * 1.1, hz - sr * 0.1 - i * sr * 0.14, sr * 2.2, (2 + i * 1.2) * U);
    // the floor
    ctx.fillStyle = "rgba(10,7,26,.72)";
    ctx.fillRect(0, hz, Wd, Ht - hz);
    ctx.strokeStyle = mix(hex(C.pink), hex(C.cyan), tension);
    ctx.globalAlpha = a * (0.28 + pulse * 0.2);
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
    // (the crowd stands low, at the foot of the stage, so its pints stay under the lyrics)
    const hop = Math.abs(Math.sin((beat + hash(i * 3) * 0.2) * Math.PI)) * (big ? 12 : 7) * U;
    const hr = (20 + hash(i * 2) * 8) * U, y = base - 52 * U - hash(i * 5) * 18 * U - hop;
    ctx.fillStyle = "rgba(12,8,20,.78)";
    ctx.beginPath(); ctx.arc(x, y, hr, 0, TAU); ctx.fill();
    roundRect(x - hr * 1.7, y + hr * 0.8, hr * 3.4, 200 * U, hr); ctx.fill();
    if (i % 2 === 0 || big) {
      const side = i % 4 < 2 ? 1 : -1;
      const sway = Math.sin(beat * Math.PI + i) * 0.25;
      const hx = x + side * hr * 1.6 + sway * 16 * U, hy = y - hr * 1.05;
      ctx.strokeStyle = "rgba(12,8,20,.78)";
      ctx.lineWidth = hr * 0.6;
      ctx.lineCap = "round";
      ctx.beginPath(); ctx.moveTo(x + side * hr * 1.2, y + hr * 1.2); ctx.lineTo(hx, hy); ctx.stroke();
      ctx.globalAlpha = a * 0.75;
      pint(hx, hy + 6 * U, 36 * U, sway * 0.5);
      ctx.globalAlpha = a;
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
  els.skies[skyOn].style.background = `linear-gradient(to bottom, rgba(8,4,16,0) 45%, rgba(8,4,16,.62) 88%), radial-gradient(ellipse at 50% 42%, rgba(0,0,0,0) 45%, rgba(8,4,16,.4)), linear-gradient(to bottom, ${th.sky[0]}, ${th.sky[1]} 55%, ${th.sky[2]})`;
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
  // the words: lit as they're sung, the one being sung filling left to right, each moving with
  // the voice from a moment before it's sung until it has settled
  if (cur >= 0) {
    const ws = lines[cur].words;
    for (let i = 0; i < ws.length; i++) {
      const w = ws[i];
      // (a shout lights up as fast as it's shouted)
      const fill = w.motion === "pop" ? Math.min(w.e - w.s, 0.16) : w.e - w.s;
      const p = t <= w.s ? 0 : t >= w.s + fill ? 1 : (t - w.s) / fill;
      if (p !== w.p) {
        w.p = p;
        if (!w.letters) w.el.style.setProperty("--p", p.toFixed(3));
        if ((p >= 1) !== w.done) { w.done = p >= 1; w.el.classList.toggle("is-done", w.done); }
      }
      if (calm) { if (w.letters) fillLetters(w, p > 0 ? 1 : 0); continue; }
      if (t > w.s - 0.02 && t < w.e + 1.1) {
        const tf = move(w, t, i);
        if (tf !== w.tf) { w.tf = tf; w.el.style.transform = tf; }
        if (w.letters) letters(w, t);
      } else {
        still(w);
        if (w.letters) fillLetters(w, p);
      }
    }
    // and the whole line breathes with the kick drum, in the loud parts
    const kind = sections[sectionAt(t)].kind;
    let beatScale = 1;
    if (!calm && !audio.paused && (kind === "chorus" || kind === "final" || kind === "outro" || kind === "bridge")) {
      const b = (t - BEAT0) / BEAT, frac = b - Math.floor(b);
      beatScale = 1 + 0.022 * Math.pow(1 - frac, 6) * Math.min(1, env(t, 0) * 1.4);
    }
    const bt = beatScale === 1 ? "" : `scale(${beatScale.toFixed(4)})`;
    const inner = lines[cur].inner;
    if (bt !== inner.tf) { inner.tf = bt; inner.style.transform = bt; }
  }
  // (lines no longer shown are reset, so a seek back finds them unlit and still)
  for (const i of [prevLine, nextLine]) {
    if (i < 0) continue;
    for (const w of lines[i].words) {
      const p = i === prevLine ? 1 : 0;
      if (w.p !== p) {
        w.p = p; w.done = p >= 1;
        if (!w.letters) w.el.style.setProperty("--p", String(p));
        fillLetters(w, p);
        w.el.classList.toggle("is-done", w.done);
      }
      still(w);
    }
    const inner = lines[i].inner;
    if (inner.tf) { inner.tf = ""; inner.style.transform = ""; }
  }
}

function resetLyrics() {
  for (const l of lines) {
    l.el.className = "ts-l";
    for (const w of l.words) { w.p = 0; w.done = false; w.el.style.setProperty("--p", "0"); fillLetters(w, 0); still(w); w.el.classList.remove("is-done"); }
    l.inner.tf = ""; l.inner.style.transform = "";
  }
  curLine = nextLine = prevLine = -1;
}

// ---------------------------------------------------------------- the words, moving with the song

// How a word moves when it's sung. Most just lift with the voice; these do something of their own,
// and any other word held for longer than half a second ripples, letter by letter, as the voice
// passes through it.
const MOTIONS = {
  pop: ["tov", "oi", "agents", "pints", "up", "bust", "ding", "five", "four", "three", "two", "one"],
  lean: ["faster", "milliseconds", "fortytwo"],
  droop: ["rust", "rusts", "poor", "compiling", "missed"],
  shrink: ["little"],
  press: ["tap", "keys"],
  hop: ["bleep"],
  shake: ["error", "never", "guesses"],
  type: ["typescript", "json", "curl", "pipe", "binary", "try", "sh", "any", "null"],
};
const MOTION = new Map();
for (const [m, ws] of Object.entries(MOTIONS)) for (const w of ws) MOTION.set(w, m);
function motionOf(text, dur) {
  const w = text.toLowerCase().replace(/[^a-z0-9]/g, "");
  let m = MOTION.get(w) || "";
  // (a pop is for the shouted words: "agent's", "one little" and "two" in "forty-two" aren't)
  if (m === "pop" && !text.includes("!") && w !== "tov" && w !== "pints") m = "";
  if (!m && dur > 0.55) m = "wave";
  return m;
}

// a nudge that rises to its height at `peak` seconds and eases away
const bump = (t, peak) => (t <= 0 ? 0 : (t / peak) * Math.exp(1 - t / peak));
// a kick to a spring: out, back past where it started, and settled (f: wobbles a second, z: damping)
const kick = (t, f, z) => (t <= 0 ? 0 : Math.exp(-z * t) * Math.sin(TAU * f * t));
const em = v => `${v.toFixed(3)}em`;

// a word's transform at `t` (its letters' too, for the ones that move by letter)
function move(w, t, i) {
  const dt = t - w.s, dur = Math.max(0.12, w.e - w.s);
  // (louder singing, bigger moves)
  const amp = 0.7 + 0.6 * env(w.s + 0.06, 3);
  const side = i % 2 ? 1 : -1;
  switch (w.motion) {
    case "pop": {
      const k = kick(dt, 2.1, 5.2);
      return `translateY(${em(-0.1 * amp * bump(dt, 0.1))}) scale(${(1 + 0.38 * amp * k).toFixed(3)}) rotate(${(side * 8 * kick(dt, 1.5, 4.5)).toFixed(2)}deg)`;
    }
    case "lean":
      return `translateX(${em(0.07 * bump(dt, 0.18))}) skewX(${(-18 * kick(dt, 1.3, 3.8)).toFixed(2)}deg)`;
    case "droop": {
      const k = inOut(dt / Math.max(dur, 0.3)) * (1 - out3((dt - dur) / 0.5));
      return `translateY(${em(0.08 * k)}) rotate(${(5 * k).toFixed(2)}deg)`;
    }
    case "shrink":
      return `scale(${(1 - 0.2 * bump(dt, 0.25)).toFixed(3)})`;
    case "press": {
      const k = kick(dt, 2.6, 6);
      return `translateY(${em(0.1 * k)}) scale(${(1 + 0.06 * k).toFixed(3)}, ${(1 - 0.12 * k).toFixed(3)})`;
    }
    case "hop":
      return `translateY(${em(-0.16 * Math.abs(kick(dt, 3, 5)))})`;
    case "shake":
      return `translateX(${em(0.05 * kick(dt, 7, 7))}) rotate(${(3 * kick(dt, 6, 7)).toFixed(2)}deg)`;
    case "wave":
    case "type":
      return `translateY(${em(-0.03 * amp * bump(dt, 0.15))})`;
    default:
      return `translateY(${em(-0.1 * amp * bump(dt, 0.12))}) scale(${(1 + 0.05 * amp * bump(dt, 0.12)).toFixed(3)})`;
  }
}

// the letters of a word that moves by letter: they fill, and lift, as the voice reaches them
function letters(w, t) {
  const n = w.letters.length, dur = Math.max(0.12, w.e - w.s);
  const voice = 0.6 + 0.8 * env(t, 3);
  for (let i = 0; i < n; i++) {
    const c = w.letters[i];
    let p, tf;
    if (w.motion === "type") {
      // (code types out: each letter lands whole, in turn)
      const lt = t - (w.s + dur * 0.8 * (i / n));
      p = lt >= 0 ? 1 : 0;
      tf = `translateY(${em(-0.06 * bump(lt, 0.06))}) scale(${(1 + 0.25 * kick(lt, 3, 9)).toFixed(3)})`;
    } else {
      // (a held note ripples through its letters)
      const x = ((t - w.s) / dur) * (n + 1) - i;
      p = clamp(x, 0, 1);
      const g = Math.exp(-((x - 0.7) * (x - 0.7)) / 0.6);
      tf = `translateY(${em(-0.2 * voice * g)}) scale(${(1 + 0.12 * g).toFixed(3)})`;
    }
    if (p !== c.p) { c.p = p; c.style.setProperty("--p", p.toFixed(3)); }
    if (tf !== c.tf) { c.tf = tf; c.style.transform = tf; }
  }
}

function still(w) {
  if (w.tf) { w.tf = ""; w.el.style.transform = ""; }
  if (w.letters) for (const c of w.letters) {
    if (c.tf) { c.tf = ""; c.style.transform = ""; }
  }
}
function fillLetters(w, p) {
  if (w.letters) for (const c of w.letters) if (c.p !== p) { c.p = p; c.style.setProperty("--p", String(p)); }
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
    floatText(`${g.label}!`, sx + (Math.random() - 0.5) * 300 * U * narrow(), sy - 140 * U, col, 44, 0.9);
    confetti(28, sx, sy, 1);
    ring(sx, sy, col, 260, 0.6, 3);
    shake(8);
    punch(0.02);
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
