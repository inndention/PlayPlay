/* PLAY PLAY web dashboard: the tab bar is wider than most windows. It scrolls inside its own cell (the brand and the clock stay
   put), the mouse wheel scrolls it sideways, the edges fade while there is more to the left or right, and the active tab is kept
   in view when the page is changed from somewhere else (a link, a key, a panel's "page ->" button). Pure presentation: no state. */
const nav = document.getElementById("tabs");
if (nav) {
  const edge = () => { nav.dataset.l = nav.scrollLeft > 2 ? "1" : "0"; nav.dataset.r = nav.scrollLeft + nav.clientWidth < nav.scrollWidth - 2 ? "1" : "0"; };
  const show = () => {
    const a = nav.querySelector("button.active"); if (!a) { edge(); return; }
    const l = a.offsetLeft - 28, r = a.offsetLeft + a.offsetWidth + 28;
    if (l < nav.scrollLeft) nav.scrollLeft = Math.max(0, l); else if (r > nav.scrollLeft + nav.clientWidth) nav.scrollLeft = r - nav.clientWidth;
    edge();
  };
  nav.addEventListener("scroll", edge, { passive: true });
  nav.addEventListener("wheel", e => { if (Math.abs(e.deltaY) > Math.abs(e.deltaX) && nav.scrollWidth > nav.clientWidth) { nav.scrollLeft += e.deltaY; e.preventDefault(); } }, { passive: false });
  window.addEventListener("resize", edge);
  new MutationObserver(show).observe(nav, { subtree: true, attributes: true, attributeFilter: ["class"] });
  if (window.ResizeObserver) new ResizeObserver(edge).observe(nav);
  show(); setTimeout(show, 400);
}
