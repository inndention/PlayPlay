'use strict';
const http = require('node:http');
const fs = require('node:fs');
const path = require('node:path');
const types = {'.html':'text/html; charset=utf-8', '.css':'text/css; charset=utf-8', '.js':'text/javascript; charset=utf-8', '.png':'image/png'};
function createRelay(root) {
  let state = {type:'star-news-control', seq:0, config:{scene:'lower'}, on:false, action:'out'};
  const server = http.createServer((req,res) => {
    const host = req.headers.host;
    if (host !== `127.0.0.1:${server.address().port}` && host !== `localhost:${server.address().port}`) { res.writeHead(403); return res.end(); }
    const pathname = new URL(req.url, 'http://127.0.0.1').pathname;
    const json = obj => { const body = JSON.stringify(obj); res.writeHead(200, {'Content-Type':'application/json','Cache-Control':'no-store'}); res.end(body); };
    if (req.method === 'GET' && pathname === '/api/health') return json({starNewsRelay:true});
    if (req.method === 'GET' && pathname === '/api/state') return json(state);
    if (req.method !== 'GET') { res.writeHead(405); return res.end(); }
    const file = pathname === '/' ? 'index.html' : pathname.slice(1);
    if (!/^(?:index\.html|output\.html|studio\.css|graphics\.css|election\.css|studio\.js|graphics\.js|assets\/star-news\.png)$/.test(file)) { res.writeHead(404); return res.end(); }
    fs.readFile(path.join(root,file),(err,body) => { if(err){res.writeHead(404);return res.end();} res.writeHead(200,{'Content-Type':types[path.extname(file)],'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'});res.end(body); });
  });
  return {server, setState(config, action) { state = {type:'star-news-control', seq:Math.max(Date.now(), state.seq+1), config:structuredClone(action === 'out' ? state.config : config), on:action !== 'out', action}; return state; }, getState(){return state;}};
}
module.exports = {createRelay};
