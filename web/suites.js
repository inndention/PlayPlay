/* PLAY PLAY web dashboard — the five suite tabs in one import for app.js:
   render (Render nodes + jobs), star (Star nodes + stations), studio (guided spot studio), publish (Publisher),
   agents (StarAgent suite). app.js calls viewSuites / onEnterSuites / onWsSuites and merges ppExtSuites into window.pp. */

import { viewRender, onEnterRender, onWsRender, ppExtRender } from "./render.js";
import { viewStar, onEnterStar, onWsStar, ppExtStar } from "./starnodes.js";
import { viewStudio, onEnterStudio, onWsStudio, ppExtStudio } from "./studio.js";
import { viewPublish, onEnterPublish, onWsPublish, ppExtPublish } from "./publisher.js";
import { viewAgents, onEnterAgents, onWsAgents, ppExtAgents } from "./agents.js";
import { viewLibObs, onEnterLibObs, onWsLibObs, ppExtLibObs } from "./libobs.js";

const VIEWS = { render: viewRender, star: viewStar, studio: viewStudio, publish: viewPublish, agents: viewAgents, libobs: viewLibObs };

/** HTML for one of the suite views ("render", "star", "studio", "publish", "agents"); "" for any other view. */
export function viewSuites(v) { const f = VIEWS[v]; return f ? f() : ""; }
/** Load that view's data when it is entered (no-op for other views). */
export function onEnterSuites(v) { onEnterRender(v); onEnterStar(v); onEnterStudio(v); onEnterPublish(v); onEnterAgents(v); onEnterLibObs(v); }
/** WS messages of type render / starnodes / spotstudio / publish / agents; true when handled (app.js then repaints). */
export function onWsSuites(m) { return !!m && (onWsRender(m) || onWsStar(m) || onWsStudio(m) || onWsPublish(m) || onWsAgents(m) || onWsLibObs(m)); }
/** Every handler the suite HTML calls through window.pp. */
import { ppExtAiModels } from "./aimodels.js";
export const ppExtSuites = Object.assign({}, ppExtRender, ppExtStar, ppExtStudio, ppExtPublish, ppExtAgents, ppExtAiModels, ppExtLibObs);
