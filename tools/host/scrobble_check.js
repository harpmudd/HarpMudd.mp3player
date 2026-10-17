/* Checks the two halves of the scrobble export that cannot be checked on the
 * Pocket: that the page decodes exactly what the firmware encodes, and that
 * the page's hand-written MD5 is really MD5.
 *
 *     node tools/host/scrobble_check.js
 *
 * Both subjects are lifted out of docs/scrobble/index.html at run time rather
 * than copied, so this cannot quietly drift from the page it is testing. The
 * encoder side mirrors qr_putenc() in fw/player.c and DOES have to be kept in
 * step by hand -- if you change the separators or the safe set there, change
 * SAFE and enc() here too, or this will cheerfully pass against the wrong
 * format.
 *
 * MD5 matters more than it looks: Last.fm signs every write with it, so an
 * implementation that is wrong only at a block boundary gives a page that
 * works on short titles and fails on long ones. The lengths below sit either
 * side of 56, 64 and 120 for that reason. */
const fs = require('fs');

const SRC = fs.readFileSync('docs/scrobble/index.html', 'utf8');

/* Pull the real functions out of the page so this cannot drift from it. */
function lift(name) {
  const i = SRC.indexOf('function ' + name + '(');
  if (i < 0) throw new Error('no ' + name + ' in the page');
  let d = 0, j = SRC.indexOf('{', i);
  const start = i;
  for (let k = j; k < SRC.length; k++) {
    if (SRC[k] === '{') d++;
    else if (SRC[k] === '}') { d--; if (!d) return SRC.slice(start, k + 1); }
  }
  throw new Error('unbalanced ' + name);
}
eval(lift('dec'));
eval(lift('parsePayload'));

/* ---- the firmware's qr_putenc(), character for character ---- */
const SAFE = new Set(
  'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~!(),'
    .split(''));
function enc(s) {
  let out = '';
  for (const b of Buffer.from(s, 'utf8')) {
    const c = String.fromCharCode(b);
    if (SAFE.has(c)) out += c;
    else if (c === ' ') out += '+';
    else out += '%' + b.toString(16).toUpperCase().padStart(2, '0');
  }
  return out;
}

/* Titles chosen to hit every branch: spaces, an apostrophe, an ampersand, a
 * '#' that would end a fragment, the separators themselves, and non-ASCII. */
const TRACKS = [
  ['The Only One I Know', 'The Charlatans'],
  ["Jumpin' Jack Flash", 'Ananda Shankar'],
  ['Rock & Roll, Part 2', 'Gary Glitter'],
  ['Track #3', 'Someone'],
  ['Star * Semi; Colon', 'Edge Case'],
  ['Björk Guðmundsdóttir', 'Björk'],
  ['100% Pure', 'Plus+Sign'],
];

let payload = '1*' + TRACKS.length + '*1;';
const want = [];
let t = 1792000000;
TRACKS.forEach(([ti, ar], i) => {
  const ts = t + i * 240;
  payload += ts + '*' + enc(ti) + '*' + enc(ar) + ';';
  want.push({ ts, title: ti, artist: ar });
});

console.log('payload bytes:', Buffer.byteLength(payload, 'utf8'));
console.log('contains whitespace:', /\s/.test(payload));

const got = parsePayload(payload);
if (!got) { console.log('FAIL: parsePayload returned null'); process.exit(1); }

let bad = 0;
if (got.total !== TRACKS.length) { console.log('FAIL total', got.total); bad++; }
if (got.realClock !== true) { console.log('FAIL realClock'); bad++; }
if (got.plays.length !== want.length) {
  console.log('FAIL count', got.plays.length, 'want', want.length); bad++;
}
want.forEach((w, i) => {
  const g = got.plays[i];
  if (!g) { console.log('FAIL missing', i); bad++; return; }
  if (g.ts !== w.ts || g.title !== w.title || g.artist !== w.artist) {
    console.log('FAIL', i, JSON.stringify(g), '!=', JSON.stringify(w));
    bad++;
  } else {
    console.log('  ok  ', g.ts, '|', g.title, '|', g.artist);
  }
});

/* The epoch=0 path must parse too, and must NOT claim a real clock. */
const boot = parsePayload('1*2*0;11*A+Song*An+Artist;251*B*C;');
if (!boot || boot.realClock !== false || boot.plays.length !== 2) {
  console.log('FAIL: boot-relative payload'); bad++;
} else console.log('  ok   boot-relative payload parses, realClock=false');

/* A payload from a future format version must be refused, not misread. */
if (parsePayload('2*1*1;11*A*B;') !== null) {
  console.log('FAIL: accepted an unknown format version'); bad++;
} else console.log('  ok   unknown format version refused');

console.log(bad ? bad + ' payload failures' : 'payload round-trips clean');

/* ---- md5, against node's own ---- */
eval(lift('md5'));
const crypto = require('crypto');
const VECTORS = [
  '', 'a', 'abc', 'message digest',
  'The quick brown fox jumps over the lazy dog',
  'abcdefghijklmnopqrstuvwxyz'.repeat(4),
  'api_keyXartistBj\u00f6rktrackGu\u00f0mundsd\u00f3ttirsecret',
];
/* Padding lands differently either side of these; a hand-written MD5 that
 * is going to be wrong is usually wrong here and nowhere else. */
for (const n of [55, 56, 57, 63, 64, 65, 119, 120]) VECTORS.push('x'.repeat(n));

let mbad = 0;
for (const v of VECTORS) {
  const mine = md5(v);
  const ref = crypto.createHash('md5').update(Buffer.from(v, 'utf8')).digest('hex');
  if (mine !== ref) {
    mbad++;
    console.log('  FAIL md5 len=' + Buffer.byteLength(v, 'utf8') +
                ' got ' + mine + ' want ' + ref);
  }
}
console.log(mbad ? mbad + ' md5 failures'
                 : 'md5 matches node crypto on ' + VECTORS.length + ' vectors');

const total = bad + mbad;
console.log(total ? '\n' + total + ' FAILURES' : '\nall clean');
process.exit(total ? 1 : 0);
