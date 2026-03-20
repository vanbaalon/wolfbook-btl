// =============================================================
// tester/server.js  —  local dev server for the box→LaTeX tester
//
// Serves tester/index.html and provides a JSON API endpoint
// that calls the C++ native addon directly.
//
// Usage:  node tester/server.js        (default port 3141)
//         PORT=4000 node tester/server.js
// =============================================================

const http  = require('http');
const fs    = require('fs');
const path  = require('path');

// Resolve addon from the project root (one level up from tester/)
const ROOT  = path.resolve(__dirname, '..');
const ADDON = require(path.join(ROOT, 'build', 'Release', 'wolfbook_btl.node'));

const PORT  = Number(process.env.PORT) || 3141;

// ----------------------------------------------------------
// Route table
// ----------------------------------------------------------
const routes = {

  // Serve the tester HTML page
  'GET /': (req, res) => {
    const html = fs.readFileSync(path.join(__dirname, 'index.html'));
    res.writeHead(200, { 'Content-Type': 'text/html; charset=utf-8' });
    res.end(html);
  },

  // POST /api/translate  { "input": "<WL box string>" }
  //  → { "latex": "...", "error": null }
  'POST /api/translate': (req, res) => {
    let body = '';
    req.on('data', chunk => { body += chunk; });
    req.on('end', () => {
      let latex = '';
      let error = null;
      try {
        const { input } = JSON.parse(body);
        if (typeof input !== 'string') throw new Error('"input" must be a string');
        // Strip WL line-continuation sequences (backslash + newline, optionally
        // surrounded by spaces) that appear when pasting from a WL session or
        // notebook.  E.g.  "...Box[...], \\\n          ..."  →  "...Box[...], ..."
        const normalised = input
          .replace(/\\\r?\n\s*/g, '')   // backslash-newline continuations
          .replace(/\r?\n\s*/g, ' ')    // bare newlines → single space
          .trim();
        // boxToLatex now returns { latex, error } — propagate the addon error
        // directly rather than catching a JS exception.
        const result = ADDON.boxToLatex(normalised);
        latex = result.latex;
        error = result.error;   // null on success, string on error
        // Debug log: print input → output so the terminal shows what was processed
        console.log('\n─── INPUT ───');
        console.log(normalised);
        console.log('─── LATEX ───');
        console.log(latex || '(empty)');
        if (error) console.log('─── ERROR ───\n' + error);
        console.log('─────────────');
      } catch (err) {
        error = String(err.message ?? err);
      }
      const payload = JSON.stringify({ latex, error });
      res.writeHead(200, {
        'Content-Type': 'application/json',
        'Access-Control-Allow-Origin': '*',
      });
      res.end(payload);
    });
  },

  // CORS pre-flight (for browsers that send OPTIONS)
  'OPTIONS /api/translate': (req, res) => {
    res.writeHead(204, {
      'Access-Control-Allow-Origin': '*',
      'Access-Control-Allow-Methods': 'POST, OPTIONS',
      'Access-Control-Allow-Headers': 'Content-Type',
    });
    res.end();
  },
};

// ----------------------------------------------------------
// Server
// ----------------------------------------------------------
const server = http.createServer((req, res) => {
  const key = `${req.method} ${req.url.split('?')[0]}`;
  const handler = routes[key];
  if (handler) {
    handler(req, res);
  } else {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end(`Not found: ${key}`);
  }
});

server.listen(PORT, '127.0.0.1', () => {
  console.log(`\nWolfbook tester running at  http://localhost:${PORT}\n`);
});
