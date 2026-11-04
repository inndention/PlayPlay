'use strict';
const {contextBridge, ipcRenderer} = require('electron');
contextBridge.exposeInMainWorld('desktop', {
  cue: (config, action) => ipcRenderer.invoke('cue', config, action),
  playplay: (settings, action) => ipcRenderer.invoke('playplay', settings, action),
  openSource: source => ipcRenderer.invoke('open-source', source),
  ncResults: params => ipcRenderer.invoke('nc-results', params)
});
