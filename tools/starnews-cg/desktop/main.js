'use strict';
const {app, BrowserWindow, ipcMain, shell, dialog} = require('electron');
const path = require('node:path');
const {createRelay} = require('./relay');
const ncResults = require('./nc-results');
const sources = {virginia:'https://www.elections.virginia.gov/results/', northcarolina:'https://www.ncsbe.gov/results-data/election-results/about-election-results-dashboard'};
let relay, port, win;
app.whenReady().then(async () => {
  relay = createRelay(path.join(__dirname, 'ui'));
  await new Promise((resolve,reject) => { relay.server.once('error',reject); relay.server.listen(8765,'127.0.0.1',resolve); });
  port = relay.server.address().port;
  win = new BrowserWindow({width:1550,height:1000,minWidth:1050,minHeight:700,backgroundColor:'#0a101a',title:'Star News CG Studio',webPreferences:{preload:path.join(__dirname,'preload.js'),contextIsolation:true,nodeIntegration:false,sandbox:true}});
  win.webContents.setWindowOpenHandler(() => ({action:'deny'}));
  win.webContents.on('will-navigate',(event,url)=>{if (!url.startsWith(`http://127.0.0.1:${port}/`)) event.preventDefault();});
  await win.loadURL(`http://127.0.0.1:${port}/`);
}).catch(err => {dialog.showErrorBox('Star News CG Studio', `Could not start the local graphics output on port 8765. ${err.message}`); app.quit();});
function validSender(event) { return event.sender === win?.webContents && event.senderFrame === win.webContents.mainFrame; }
ipcMain.handle('cue',(event,config,action)=>{
  if (!validSender(event) || !config || typeof config !== 'object' || JSON.stringify(config).length > 20000 || !['in','out','replay'].includes(action)) throw Error('Invalid cue');
  return relay.setState(config,action);
});
ipcMain.handle('open-source',(event,source)=> {if(!validSender(event) || (!sources[source] && source !== 'output')) throw Error('Unknown source'); return shell.openExternal(source === 'output' ? `http://127.0.0.1:${port}/output.html?live=1` : sources[source]);});
ipcMain.handle('nc-results',(event,params)=>{if(!validSender(event) || !params || typeof params.date!=='string' || typeof params.county!=='string' || typeof params.contest!=='string' || params.county.length>80 || params.contest.length>160) throw Error('Invalid results query');return ncResults.download(params.date,params.county,params.contest);});
ipcMain.handle('playplay', async (event, settings, action) => {
  if(!validSender(event) || !settings || !['show','hide'].includes(action)) throw Error('Invalid request');
  const ppPort=Number(settings.port), layer=Number(settings.layer), channel=String(settings.channel || '');
  if(!Number.isInteger(ppPort)||ppPort<1||ppPort>65535||!Number.isInteger(layer)||layer<1||layer>999||!/^[-\w]{1,80}$/.test(channel)) throw Error('Enter a valid Supervisor port, channel ID and layer (1–999).');
  const body = action === 'show' ? {action:'show',layer,role:'secondary',source:{type:'html',url:`http://127.0.0.1:${port}/output.html?live=1`,width:1920,height:1080,transparent:true},placement:{x:0,y:0,width:1920,height:1080},in:{type:'cut'}} : {action:'hide',layer,transition:{type:'fade',durationSec:0.3}};
  const headers={'Content-Type':'application/json'}; if(settings.token) headers.Authorization='Bearer '+String(settings.token);
  const response=await fetch(`http://127.0.0.1:${ppPort}/api/v1/channels/${encodeURIComponent(channel)}/cg`,{method:'POST',headers,body:JSON.stringify(body),signal:AbortSignal.timeout(5000)});
  const result=await response.json(); if(!response.ok || result.error || result.ok === false) throw Error(result.error || `Supervisor returned ${response.status}`);
  return result;
});
app.on('window-all-closed',()=>{ relay?.server.close(); app.quit(); });
