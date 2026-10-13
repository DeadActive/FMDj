// SPDX-License-Identifier: GPL-3.0-only
// LSDj fork: 2026 DEADACTIVE
// The song page's link (web/lsdjlink.js Link: it talks to an FM-1's console over USB) pointed at the simulator: what
// it sends is cut into lines, each is given to the simulated firmware's console, and what that prints comes back the
// way the real console sends it (the line echoed, the output, a prompt). So the same code lists, uploads and
// downloads songs and saves here.
// Link: web/lsdjlink.js's class; ask: async (line) -> the firmware's output for it (engine.js console(), through the
// worklet on the page)
export function simLink(Link, ask, timeoutMs = 4000) {
  let typed = '';
  const say = text => link.feed(Uint8Array.from(text, c => c.charCodeAt(0)));
  const link = new Link(async bytes => {
    for (const b of bytes) {
      if (b !== 13) { typed += String.fromCharCode(b); continue; }
      const line = typed;
      typed = '';
      say(line ? `${line}\r\n${await ask(line)}> ` : '\r\n> ');
    }
  }, timeoutMs);
  return link;
}
