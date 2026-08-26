/* ==========================================================================
   cpp-todo — 前端单页应用
   六大视图：今日 / 项目 / 标签 / 日历 / 看板 / 保存筛选
   对接后端 REST API（/api/*），Markdown 备注用本地 marked.min.js 渲染
   ========================================================================== */
'use strict';

/* ---------------- 工具函数 ---------------- */
const $  = (s, c) => (c || document).querySelector(s);
const $$ = (s, c) => Array.from((c || document).querySelectorAll(s));

function esc(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

function toast(msg, type) {
  const t = document.createElement('div');
  t.className = 'toast' + (type ? ' ' + type : '');
  var ico = type === 'ok' ? '\u2713 ' : type === 'err' ? '\u26a0 ' : '';
  t.innerHTML = ico ? '<span class="toast-ico">' + ico.trim() + '</span>' : '';
  t.appendChild(document.createTextNode(msg));
  $('#toast-root').appendChild(t);
  setTimeout(() => { t.classList.add('toast-out'); }, 2600);
  setTimeout(() => t.remove(), 3000);
}

function fmtDate(iso) {
  if (!iso) return '';
  const p = String(iso).split('-');
  if (p.length < 3) return iso;
  return parseInt(p[1], 10) + '月' + parseInt(p[2], 10) + '日';
}

function daysBetween(fromISO, toISO) {
  const [y1, m1, d1] = fromISO.split('-').map(Number);
  const [y2, m2, d2] = toISO.split('-').map(Number);
  return Math.round((Date.UTC(y2, m2 - 1, d2) - Date.UTC(y1, m1 - 1, d1)) / 86400000);
}

function md(text) {
  const s = text || '';
  if (window.marked) {
    try { return marked.parse ? marked.parse(s) : marked(s); } catch (e) { return esc(s); }
  }
  return esc(s).replace(/\n/g, '<br>');
}

const PRIO = {
  2: { label: '高', cls: 'prio-high' },
  1: { label: '中', cls: 'prio-mid' },
  0: { label: '低', cls: 'prio-low' }
};
const STATUS_LABEL = { todo: '待办', doing: '进行中', done: '已完成', archived: '已归档' };

function repeatText(rule) {
  if (!rule || !rule.freq) return '';
  const freqCn = { daily: '天', weekly: '周', monthly: '月', yearly: '年', custom: '周期' }[rule.freq] || '';
  let t = '每';
  if (rule.interval && rule.interval > 1) t += rule.interval;
  t += freqCn;
  if (rule.weekdays && rule.weekdays.length && (rule.freq === 'weekly' || rule.freq === 'custom')) {
    const WD = ['', '一', '二', '三', '四', '五', '六', '日'];
    t += ' · ' + rule.weekdays.map(w => '周' + WD[w]).join('/');
  }
  const sk = [];
  if (rule.skipWeekends) sk.push('周末');
  if (rule.skipHolidays) sk.push('节假日');
  if (sk.length) t += ' · 跳过' + sk.join('/');
  return t;
}

/* ---------------- API 层 ---------------- */
async function api(method, path, body) {
  const opt = { method, headers: {} };
  if (body !== undefined) {
    opt.headers['Content-Type'] = 'application/json';
    opt.body = JSON.stringify(body);
  }
  const r = await fetch(path, opt);
  let data = null;
  try { data = await r.json(); } catch (e) { /* 非 JSON */ }
  if (!r.ok) {
    const msg = (data && data.error) ? data.error : ('HTTP ' + r.status);
    throw new Error(msg);
  }
  return data;
}
const apiGet = (p) => api('GET', p);

/* 按钮加载态：注入旋转图标，禁用按钮，返回恢复函数 */
function btnLoading(btn) {
  var orig = btn.innerHTML;
  btn.classList.add('is-loading');
  btn.innerHTML = '<span class="btn-spinner"></span>' + orig;
  return function() {
    btn.classList.remove('is-loading');
    btn.innerHTML = orig;
  };
}

/* ---------------- 图标辅助 ---------------- */
function svgIcon(name, size) {
  size = size || 16;
  return '<svg width="' + size + '" height="' + size + '" style="vertical-align:-2px;fill:none;stroke:currentColor;stroke-width:1.75;stroke-linecap:round;stroke-linejoin:round"><use href="#icon-' + name + '"/></svg>';
}

/* ---------------- 全局状态 ---------------- */
const S = {
  view: 'today',
  today: '',
  lunarToday: '',
  projects: [],          // 扁平列表
  tags: [],
  filters: [],
  calYear: 0,
  calMonth: 0,
  calMode: 'month',      // month | week
  weekOffset: 0,         // 周视图相对本周的偏移
  selectedProject: null, // 项目视图当前选中（0=未分类）
  selectedTag: null,
  expanded: new Set(),   // 项目视图展开的文件夹
  taskIndex: new Map(),  // id → 任务（供下拉选择）
  searchTimer: null,
  batchMode: false,      // 批量选择模式
  batchSel: new Set(),   // 已勾选任务 id
  dragTaskId: null,      // 拖拽中的任务 id
  heatYear: 0,           // 热力图当前年份（0=今年）
  dayDate: '',           // 日视图当前日期（空=今天）
  lastNewTaskId: null    // 新建任务后用于触发淡入动画
};

/* ---------------- 初始化 ---------------- */
async function init() {
  initTheme();
  try {
    const meta = await apiGet('/api/meta');
    S.today = meta.today;
    S.lunarToday = meta.lunarToday || '';
    $('#lunar-today').textContent = '农历 ' + S.lunarToday;
    const [y, m] = meta.today.split('-').map(Number);
    S.calYear = y; S.calMonth = m;
  } catch (e) {
    $('#lunar-today').textContent = '服务未就绪';
  }
  bindEvents();
  bindPomodoro();
  await refreshSidebar();
  await switchView('today');
}

/* ---------------- 主题（深色模式） ---------------- */
function initTheme() {
  try {
    if (localStorage.getItem('todo-theme') === 'dark')
      document.documentElement.classList.add('dark');
  } catch (e) { /* 无 localStorage 时忽略 */ }
}

function toggleTheme() {
  document.documentElement.classList.toggle('dark');
  try {
    localStorage.setItem('todo-theme',
      document.documentElement.classList.contains('dark') ? 'dark' : 'light');
  } catch (e) { /* 忽略 */ }
  toast(document.documentElement.classList.contains('dark') ? '已切换深色模式' : '已切换浅色模式');
}

async function refreshSidebar() {
  try {
    const [pj, tg] = await Promise.all([apiGet('/api/projects'), apiGet('/api/tags')]);
    S.projects = pj.projects || [];
    S.inbox = pj.inbox;
    S.tags = tg.tags || [];
  } catch (e) { /* 忽略 */ }
  renderProjectTree();
  renderTagCloud();
}

/* 侧边栏项目树（扁平 → 嵌套） */
function buildPTree(list) {
  const map = new Map(list.map(p => [p.id, Object.assign({ children: [] }, p)]));
  const roots = [];
  for (const p of map.values()) {
    if (p.parentId && map.has(p.parentId)) map.get(p.parentId).children.push(p);
    else roots.push(p);
  }
  const sort = a => { a.children.sort((x, y) => x.name.localeCompare(y.name, 'zh')); a.children.forEach(sort); };
  sort({ children: roots });
  return roots;
}

function pnodeHTML(p, depth) {
  const hasKids = p.children && p.children.length;
  const isFolder = p.isFolder || hasKids;
  const open = S.expanded.has('p' + p.id);
  const kids = (isFolder && open)
    ? '<ul>' + p.children.map(c => pnodeHTML(c, depth + 1)).join('') + '</ul>' : '';
  const caret = isFolder
    ? '<span class="pcaret">' + (open ? '▼' : '▶') + '</span>'
    : '<span class="pcaret"></span>';
  const sel = S.view === 'projects' && S.selectedProject === p.id ? ' selected' : '';
  return '<li>' +
    '<button class="pnode' + (isFolder ? ' pfolder' : '') + sel + '" data-act="proj" data-id="' + p.id +
    '" data-folder="' + (isFolder ? 1 : 0) + '">' +
    caret +
    '<span class="pdot" style="background:' + esc(p.color) + '"></span>' +
    '<span class="pname">' + esc(p.name) + '</span>' +
    (p.taskCount ? '<span class="pcnt">' + p.taskCount + '</span>' : '') +
    '</button>' + kids + '</li>';
}

function renderProjectTree() {
  const box = $('#project-tree');
  const roots = buildPTree(S.projects);
  const selInbox = S.view === 'projects' && S.selectedProject === 0;
  let html = '<ul class="ptree"><li><button class="pnode' + (selInbox ? ' selected' : '') +
    '" data-act="proj" data-id="0" data-folder="0">' +
    '<span class="pcaret"></span>' +
    '<span class="pdot" style="background:#9AA0A6"></span>' +
    '<span class="pname">未分类</span>' +
    (S.inbox && S.inbox.taskCount ? '<span class="pcnt">' + S.inbox.taskCount + '</span>' : '') +
    '</button></li>' +
    roots.map(p => pnodeHTML(p, 0)).join('') + '</ul>';
  box.innerHTML = html;
}

function renderTagCloud() {
  const box = $('#tag-cloud');
  box.innerHTML = S.tags.length
    ? S.tags.map(t =>
        '<button class="tag-chip" data-act="tag" data-name="' + esc(t.name) + '">' +
        '<span class="dot" style="background:' + esc(t.color) + '"></span>' +
        esc(t.name) + (t.count ? '<span class="cnt">' + t.count + '</span>' : '') +
        '</button>').join('')
    : '<div style="color:var(--text-3);font-size:12px;padding:4px 8px">暂无标签</div>';
}

/* ---------------- 视图切换 ---------------- */

/* 根据当前视图生成骨架屏 HTML */
function skeletonHTML(view) {
  if (view === 'stats') {
    return '<div class="sk-wrap"><div class="sk-cards">' +
      Array(5).fill('<div class="sk-statcard"><div class="sk-line mid"></div><div class="sk-line short"></div></div>').join('') +
      '</div><div class="sk-card" style="padding:16px"><div class="sk-line"></div><div class="sk-line mid"></div><div class="sk-line short"></div></div></div>';
  }
  if (view === 'kanban') {
    return '<div class="kanban">' + Array(3).fill(
      '<div class="kb-col"><div class="kb-col-head"><span class="sk-line mid" style="margin:0"></span></div><div class="kb-list">' +
      Array(3).fill('<div class="kb-card" style="pointer-events:none"><div class="sk-line mid"></div><div class="sk-line short"></div></div>').join('') +
      '</div></div>'
    ).join('') + '</div>';
  }
  if (view === 'calendar') {
    return '<div class="cal-grid" style="pointer-events:none">' +
      Array(35).fill('<div class="cal-cell blank" style="min-height:86px"><div class="sk-line short"></div></div>').join('') + '</div>';
  }
  // 默认：列表型骨架
  return '<div class="sk-wrap"><div class="sk-head"><div class="sk-line"></div></div>' +
    '<div class="sk-card">' + Array(5).fill(
      '<div class="sk-row"><div class="sk-check sk-line" style="margin:0"></div><div style="flex:1"><div class="sk-line mid"></div><div class="sk-line short"></div></div></div>'
    ).join('') + '</div></div>';
}

/* ---------------- 入场动效 ---------------- */
/* 视图切换后：内容区块、任务行依次滑入淡入；交错延迟经 CSS 变量 --d 传给样式 */
function animateEnter(content) {
  const blocks = content.children;
  for (let i = 0; i < blocks.length; i++) {
    const base = Math.min(i * 55, 380);           // 区块交错步长 55ms，封顶 380ms
    blocks[i].classList.add('anim-enter');
    blocks[i].style.setProperty('--d', base + 'ms');
    const rows = blocks[i].querySelectorAll('.task-row');
    for (let r = 0; r < rows.length; r++) {       // 卡片内任务行二级交错（仅前 9 行，长列表不再逐行等待）
      rows[r].classList.add('anim-row');
      rows[r].style.setProperty('--d', base + 90 + Math.min(r, 8) * 40 + 'ms');
    }
  }
}

async function switchView(v) {
  // 首次加载或视图真正切换时播入场动效；refreshAll() 的同视图重绘不播，避免闪烁
  const animated = (v !== S.view) || !S.booted;
  S.booted = true;
  S.view = v;
  $$('#view-nav .nav-item').forEach(b => b.classList.toggle('active', b.dataset.view === v));
  const content = $('#content');
  // 骨架屏占位（快速感知加载）
  content.innerHTML = skeletonHTML(v);
  // 包装 appendChild：首次追加真实内容时自动清除骨架屏
  var cleared = false;
  var origAppend = content.appendChild.bind(content);
  content.appendChild = function(child) {
    if (!cleared) { content.innerHTML = ''; cleared = true; content.appendChild = origAppend; }
    return origAppend(child);
  };
  try {
    if (v === 'today')     await renderToday(content);
    if (v === 'projects')  await renderProjects(content);
    if (v === 'tags')      await renderTags(content);
    if (v === 'calendar')  await renderCalendar(content);
    if (v === 'day')       await renderDay(content);
    if (v === 'kanban')    await renderKanban(content);
    if (v === 'filters')   await renderFilters(content);
    if (v === 'stats')     await renderStats(content);
    if (v === 'gantt')     await renderGantt(content);
    if (v === 'trash')     await renderTrash(content);
  } catch (e) {
    content.innerHTML = '<div class="loading"><span class="spin-ico"></span>加载失败：' + esc(e.message) + '</div>';
  }
  // 确保 appendChild 已恢复
  if (!cleared) { content.appendChild = origAppend; }
  if (animated) animateEnter(content);
}

function viewHead(title, sub, extra) {
  const h = document.createElement('div');
  h.className = 'view-head';
  h.innerHTML = '<h2>' + esc(title) + '</h2>' +
    (sub ? '<span class="sub">' + esc(sub) + '</span>' : '') +
    (extra || '') ;
  return h;
}

/* ---------------- 任务组件 ---------------- */

/* 任务行右侧 meta 徽标 */
function taskMeta(t) {
  const chips = [];
  const p = PRIO[t.priority] || PRIO[1];
  chips.push('<span class="m-chip ' + p.cls + '">' + p.label + '</span>');

  if (t.dueDate) {
    const over = daysBetween(S.today, t.dueDate) < 0 && t.status !== 'done';
    chips.push('<span class="m-chip due' + (over ? ' due-overdue' : '') + '">' +
      '截止 ' + fmtDate(t.dueDate) + (over ? ' · 逾期' + -daysBetween(S.today, t.dueDate) + '天' : '') + '</span>');
  }
  if (t.startDate) {
    chips.push('<span class="m-chip">开始 ' + fmtDate(t.startDate) + '</span>');
  }
  if (t.lunarRemind && t.lunarDate) {
    chips.push('<span class="m-chip lunar">🏮 ' + esc(t.lunarText || t.lunarDate) + '</span>');
  }
  if (t.repeatRule && t.repeatRule.freq) {
    chips.push('<span class="m-chip repeat">↻ ' + esc(repeatText(t.repeatRule)) + '</span>');
  }
  if (t.project) {
    chips.push('<span class="m-chip project"><span class="dot" style="background:' + esc(t.project.color) + '"></span>' +
      esc(t.project.name) + '</span>');
  }
  if (t.status === 'doing') {
    chips.push('<span class="m-chip" style="color:#b54708;background:#fffaeb;border-color:#fedf89">进行中</span>');
  }
  for (const tg of t.tags || []) {
    chips.push('<span class="m-chip"><span class="dot" style="background:' + esc(tg.color) + '"></span>' +
      esc(tg.name) + '</span>');
  }
  return chips.join('');
}

/* 单行任务（今日/列表/项目/标签/筛选共用） */
function taskRowHTML(t, depth) {
  const d = depth || 0;
  const done = t.status === 'done';
  const checkIco = done ? '✓' : '';
  const actions =
    '<div class="t-actions">' +
    '<button class="t-btn" data-act="addsub" data-id="' + t.id + '" title="添加子任务">' + svgIcon('plus', 12) + '</button>' +
    '<button class="t-btn" data-act="edit" data-id="' + t.id + '" title="编辑">✎</button>' +
    '<button class="t-btn" data-act="pomo" data-id="' + t.id + '" title="用番茄钟专注此任务">' + svgIcon('pomodoro', 12) + '</button>' +
    '<button class="t-btn" data-act="focus" data-id="' + t.id + '" title="专注模式（全屏番茄钟）">' + svgIcon('focus', 12) + '</button>' +
    '<button class="t-btn danger" data-act="del" data-id="' + t.id + '" title="删除">🗑</button>' +
    '</div>';

  let notesPreview = '';
  if (t.notes) {
    const plain = t.notes.replace(/[#*`>_\[\]~-]/g, '').replace(/\s+/g, ' ').trim();
    if (plain) notesPreview = '<div class="t-notes-preview">' + esc(plain.slice(0, 80)) + '</div>';
  }

  const subCount = (t.children && t.children.length) ? '<span class="m-chip">子任务 ' + t.children.length + '</span>' : '';
  const pomoCnt = t.pomodoros ? '<span class="m-chip">' + svgIcon('pomodoro', 12) + ' ' + t.pomodoros + '</span>' : '';
  const selBox = S.batchMode
    ? '<input type="checkbox" class="t-sel" data-act="sel" data-id="' + t.id + '"' +
      (S.batchSel.has(t.id) ? ' checked' : '') + '>'
    : '';
  const draggable = S.batchMode ? '' : ' draggable="true"';

  return '<div class="task-row status-' + esc(t.status) + ' depth-' + Math.min(d, 6) +
    (t.blocked ? ' blocked' : '') + '" data-id="' + t.id + '"' + draggable + '>' +
    selBox +
    '<div class="t-check" data-act="' + (done ? 'reopen' : 'done') + '" data-id="' + t.id + '">' + checkIco + '</div>' +
    '<div class="t-main">' +
    '<div class="t-title">' + esc(t.title) + '</div>' +
    notesPreview +
    '<div class="t-meta">' + taskMeta(t) + subCount + pomoCnt + '</div>' +
    '</div>' + actions +
    '</div>';
}

/* 嵌套子任务（树形渲染） */
function taskTreeHTML(node, depth) {
  let html = taskRowHTML(node, depth);
  if (node.children && node.children.length) {
    html += node.children.map(c => taskTreeHTML(c, depth + 1)).join('');
  }
  return html;
}

function buildTaskIndex(list, map) {
  for (const t of list) {
    map.set(t.id, t);
    if (t.children) buildTaskIndex(t.children, map);
  }
}

/* 任务列表容器（卡片内） */
function taskListHTML(tasks, { tree = false, none = '暂无任务' } = {}) {
  if (!tasks || !tasks.length) return '<div class="empty-tip">' + esc(none) + '</div>';
  if (tree) return '<div class="list">' + tasks.map(t => taskTreeHTML(t, 0)).join('') + '</div>';
  return '<div class="list">' + tasks.map(t => taskRowHTML(t, 0)).join('') + '</div>';
}

/* ---------------- 今日视图 ---------------- */
async function renderToday(content) {
  const [data, digestData] = await Promise.all([
    apiGet('/api/today'),
    apiGet('/api/digest').catch(() => null)
  ]);
  const groups = [
    { key: 'overdue',   title: '已逾期',   badge: 'over', cls: 'b-red',   none: '没有逾期任务' },
    { key: 'dueToday',  title: '今日到期', badge: '',      cls: 'b-amber', none: '今日无到期任务' },
    { key: 'startToday', title: '今日开始', badge: '',     cls: 'b-green', none: '今日无开始任务' },
    { key: 'noDate',    title: '无日期',   badge: '',      cls: '',        none: '无日期任务都清空了' },
    { key: 'doneToday', title: '今日已完成', badge: data.doneToday.length, cls: 'b-green', none: '今天还没完成任务', muted: true }
  ];
  const head = viewHead('今日',
    S.today ? (S.today.slice(0, 4) + '年' + parseInt(S.today.slice(5, 7), 10) + '月' +
      parseInt(S.today.slice(8, 10), 10) + '日 · 农历 ' + S.lunarToday) : '',
    '<div class="spacer" style="flex:1"></div>' +
    '<button class="batch-toggle' + (S.batchMode ? ' on' : '') + '" data-act="batch-toggle">' +
      (S.batchMode ? '✓ 批量模式（点击退出）' : '批量操作') + '</button>' +
    '<button class="batch-toggle" data-act="open-pomo">' + svgIcon('pomodoro', 12) + ' 番茄钟</button>');
  content.appendChild(head);

  // 每日摘要横幅
  if (digestData && digestData.digest) {
    const banner = document.createElement('div');
    banner.className = 'digest-banner';
    banner.innerHTML = '<span class="db-ico">📋</span><div>' +
      esc(digestData.digest).replace(/\*\*(.+?)\*\*/g, '<b>$1</b>') + '</div>';
    content.appendChild(banner);
  }

  // 批量操作条
  if (S.batchMode) content.appendChild(buildBatchBar());

  let total = 0;
  const counts = { overdue: 0, dueToday: 0, startToday: 0, noDate: 0, doneToday: 0 };
  for (const g of groups) {
    counts[g.key] = (data[g.key] || []).length;
    total += counts[g.key];
  }
  const summary = document.createElement('div');
  summary.style.cssText = 'display:flex;gap:10px;flex-wrap:wrap;margin-bottom:16px';
  summary.innerHTML = [
    '<span class="m-chip due-overdue">逾期 ' + counts.overdue + '</span>',
    '<span class="m-chip due">今日 ' + counts.dueToday + '</span>',
    '<span class="m-chip">开始 ' + counts.startToday + '</span>',
    '<span class="m-chip">无日期 ' + counts.noDate + '</span>',
    '<span class="m-chip" style="color:var(--ok)">已完成 ' + counts.doneToday + '</span>',
    '<span class="m-chip">共 ' + total + '</span>'
  ].join('');
  content.appendChild(summary);

  for (const g of groups) {
    const card = document.createElement('div');
    card.className = 'group';
    card.innerHTML = '<div class="group-title"><span>' + esc(g.title) + '</span>' +
      '<span class="badge ' + g.cls + '">' + counts[g.key] + '</span></div>' +
      '<div class="card">' + taskListHTML(data[g.key], { none: g.none }) + '</div>';
    content.appendChild(card);
  }
}

/* ---------------- 批量操作 ---------------- */
function buildBatchBar() {
  const bar = document.createElement('div');
  bar.className = 'batch-bar';
  bar.innerHTML =
    '<span class="bb-count">' + S.batchSel.size + '</span><span class="bb-title">项已选</span>' +
    '<button class="btn btn-sm btn-primary" data-act="batch-go" data-op="complete">完成</button>' +
    '<button class="btn btn-sm" data-act="batch-go" data-op="reopen">重开</button>' +
    '<button class="btn btn-sm" data-act="batch-go" data-op="delete">移入回收站</button>' +
    '<select id="batch-project"><option value="">移动到项目…</option><option value="0">未分类</option>' +
    S.projects.map(p => '<option value="' + p.id + '">' + esc(p.name) + '</option>').join('') +
    '</select>' +
    '<input id="batch-tag" placeholder="添加标签…" style="padding:5px 8px;border:1px solid var(--border);border-radius:6px;width:110px">' +
    '<button class="btn btn-sm" data-act="batch-tag">打标签</button>' +
    '<button class="btn btn-sm" data-act="batch-clear">清空选择</button>';
  return bar;
}

async function runBatch(action, extra) {
  if (!S.batchSel.size) { toast('请先勾选任务', 'err'); return; }
  const body = Object.assign({ action: action, ids: Array.from(S.batchSel) }, extra || {});
  try {
    const r = await api('POST', '/api/tasks/batch', body);
    toast('批量' + action + ' 完成，影响 ' + r.affected + ' 项', 'ok');
    S.batchSel.clear();
    await refreshAll();
  } catch (e) { toast(e.message, 'err'); }
}

/* ---------------- 项目视图 ---------------- */
async function renderProjects(content) {
  const data = await apiGet('/api/projects');
  S.projects = data.projects || [];
  S.inbox = data.inbox;
  renderProjectTree();

  const sel = S.selectedProject;
  const head = viewHead('项目', '文件夹式组织任务');
  content.appendChild(head);

  // 项目树卡片（文件夹可展开）
  const treeCard = document.createElement('div');
  treeCard.className = 'card proj-tree-card';
  const roots = buildPTree(S.projects);

  function folderHTML(p) {
    const hasKids = p.children && p.children.length;
    const isFolder = p.isFolder || hasKids;
    const open = S.expanded.has('p' + p.id);
    let html = '<div class="' + (isFolder ? 'folder-row' : 'proj-row') + '" data-act="proj" data-id="' +
      p.id + '" data-folder="' + (isFolder ? 1 : 0) + '">';
    if (isFolder) {
      html += '<span class="caret">' + (open ? '▼' : '▶') + '</span>';
    } else {
      html += '<span class="dot" style="background:' + esc(p.color) + '"></span>';
    }
    html += '<span>' + esc(p.name) + '</span>' +
      '<span class="cnt">' + (p.taskCount || 0) + ' 项</span></div>';
    if (isFolder && open && p.children.length) {
      html += p.children.map(folderHTML).join('');
    }
    return html;
  }
  const inboxHTML = '<div class="proj-row' + (sel === 0 ? '' : '') + '" data-act="proj" data-id="0" data-folder="0">' +
    '<span class="dot" style="background:#9AA0A6"></span><span>未分类</span>' +
    '<span class="cnt">' + (S.inbox ? S.inbox.taskCount : 0) + ' 项</span></div>';
  treeCard.innerHTML = inboxHTML + roots.map(folderHTML).join('');
  content.appendChild(treeCard);

  // 选中项目的任务
  if (sel !== null && sel !== undefined) {
    const title = sel === 0 ? '未分类' :
      (S.projects.find(p => p.id === sel) || {}).name || ('项目 ' + sel);
    const sec = document.createElement('div');
    sec.className = 'group';
    const tasks = await apiGet('/api/tasks?project=' + sel + '&include_children=1&parent=0');
    sec.innerHTML = '<div class="group-title"><span>『' + esc(title) + '』 任务</span>' +
      '<span class="badge">' + tasks.count + '</span></div>' +
      '<div class="card">' + taskListHTML(tasks.tasks, { tree: true, none: '该项目暂无任务' }) + '</div>';
    content.appendChild(sec);
  }
}

/* ---------------- 标签视图 ---------------- */
async function renderTags(content) {
  const data = await apiGet('/api/tags');
  S.tags = data.tags || [];
  renderTagCloud();

  const head = viewHead('标签', '按标签筛选任务');
  content.appendChild(head);

  const grid = document.createElement('div');
  grid.className = 'tag-grid';
  grid.innerHTML = S.tags.length
    ? S.tags.map(t =>
        '<button class="tag-tile" data-act="tag" data-name="' + esc(t.name) + '">' +
        '<span class="dot" style="background:' + esc(t.color) + '"></span>' +
        '<span class="name">' + esc(t.name) + '</span>' +
        '<span class="count">' + (t.count || 0) + '</span></button>').join('')
    : '<div class="empty-tip" style="grid-column:1/-1">暂无标签。新建任务时可添加标签。</div>';
  content.appendChild(grid);

  if (S.selectedTag) {
    const sec = document.createElement('div');
    sec.className = 'group';
    const tasks = await apiGet('/api/tasks?tag=' + encodeURIComponent(S.selectedTag) + '&include_children=1&parent=0');
    sec.innerHTML = '<div class="group-title"><span>『' + esc(S.selectedTag) + '』 任务</span>' +
      '<span class="badge">' + tasks.count + '</span></div>' +
      '<div class="card">' + taskListHTML(tasks.tasks, { tree: true, none: '该标签下暂无任务' }) + '</div>';
    content.appendChild(sec);
  }
}

/* ---------------- 日历视图（月 / 周） ---------------- */
const CAL_DOW = ['一', '二', '三', '四', '五', '六', '日'];

function mondayOf(iso, offsetWeeks, extraDays) {
  const [y, m, d] = iso.split('-').map(Number);
  const dt = new Date(y, m - 1, d);
  const dow = (dt.getDay() + 6) % 7; // 0=周一
  dt.setDate(dt.getDate() - dow + (offsetWeeks || 0) * 7 + (extraDays || 0));
  const p = n => String(n).padStart(2, '0');
  return dt.getFullYear() + '-' + p(dt.getMonth() + 1) + '-' + p(dt.getDate());
}

async function renderCalendar(content) {
  const weekMode = S.calMode === 'week';
  const head = viewHead('日历', '含农历、节气与节假日标注');
  content.appendChild(head);

  const toolbar = document.createElement('div');
  toolbar.className = 'cal-toolbar week-toolbar';
  let title = S.calYear + '年' + S.calMonth + '月';
  if (weekMode) {
    const ws = mondayOf(S.today, S.weekOffset);
    const we = mondayOf(S.today, S.weekOffset, 6);
    title = ws.slice(0, 4) + '年' + parseInt(ws.slice(5, 7), 10) + '月' +
      parseInt(ws.slice(8, 10), 10) + '日 – ' +
      (we.slice(5, 7) === ws.slice(5, 7) ? '' : parseInt(we.slice(5, 7), 10) + '月') +
      parseInt(we.slice(8, 10), 10) + '日';
  }
  toolbar.innerHTML =
    '<button class="btn btn-sm" data-act="cal-prev">' + (weekMode ? '‹ 上周' : '‹ 上月') + '</button>' +
    '<span class="month-title">' + title + '</span>' +
    '<button class="btn btn-sm" data-act="cal-next">' + (weekMode ? '下周 ›' : '下月 ›') + '</button>' +
    '<button class="btn btn-sm" data-act="cal-today">回到今天</button>' +
    '<div class="seg">' +
    '<button data-act="cal-mode" data-mode="month"' + (weekMode ? '' : ' class="on"') + '>月</button>' +
    '<button data-act="cal-mode" data-mode="week"' + (weekMode ? ' class="on"' : '') + '>周</button>' +
    '</div>' +
    '<div class="spacer" style="flex:1"></div>' +
    '<button class="btn btn-sm" data-act="cal-holidays" title="按农历/节气推导，生成本年春节、清明、端午、中秋等法定节假日">🏮 生成节假日</button>' +
    '<button class="btn btn-sm" data-act="new" title="新建任务">＋ 新建</button>';
  content.appendChild(toolbar);

  if (weekMode) {
    await renderWeekView(content);
    return;
  }

  const data = await apiGet('/api/calendar?year=' + S.calYear + '&month=' + S.calMonth);
  const firstDay = new Date(S.calYear, S.calMonth - 1, 1).getDay(); // 0=周日
  const offset = (firstDay + 6) % 7; // 周一开头

  const grid = document.createElement('div');
  grid.className = 'cal-grid';
  grid.innerHTML = CAL_DOW.map((d, i) =>
    '<div class="cal-dow' + (i >= 5 ? ' weekend' : '') + '">' + d + '</div>').join('');

  for (let i = 0; i < offset; i++) {
    grid.insertAdjacentHTML('beforeend', '<div class="cal-cell blank"></div>');
  }

  for (const day of data.days || []) {
    const isWeekend = new Date(day.date).getDay() === 0 || new Date(day.date).getDay() === 6;
    const isToday = day.date === S.today;
    const dayNum = parseInt(day.date.slice(8, 10), 10);
    const kindDot = {
      due: '<span class="cal-dot due" title="到期"></span>',
      start: '<span class="cal-dot start" title="开始"></span>',
      lunar: '<span class="cal-dot lunar" title="农历提醒"></span>'
    };
    let dots = '';
    let shown = 0;
    let repeatCount = 0;
    for (const e of day.tasks || []) {
      if (e.task.recurringInstance) repeatCount++;
      const dot = e.kind === 'start' ? kindDot.start
        : e.kind === 'lunar' ? kindDot.lunar : kindDot.due;
      if (shown < 5) { dots += dot; shown++; }
    }
    if (repeatCount) dots += '<span class="cal-dot repeat" title="重复实例"></span>';
    const more = (day.tasks || []).length > 5
      ? '<div class="d-more">+' + ((day.tasks || []).length - 5) + '</div>' : '';

    grid.insertAdjacentHTML('beforeend',
      '<div class="cal-cell' + (isToday ? ' today' : '') + (isWeekend ? ' weekend' : '') +
      '" data-act="cal-day" data-date="' + day.date + '">' +
      (day.isHoliday ? '<span style="position:absolute;top:4px;right:5px;color:var(--danger);font-size:10px">节</span>' : '') +
      '<div class="d-num">' + dayNum + '</div>' +
      '<div class="d-lunar' + (day.isHoliday ? ' d-holiday' : '') + '">' + esc(day.lunar || '') + '</div>' +
      (dots ? '<div class="d-tasks">' + dots + '</div>' : '') + more +
      '</div>');
  }
  content.appendChild(grid);

  const legend = document.createElement('div');
  legend.className = 'cal-legend';
  legend.innerHTML =
    '<span class="lg"><span class="cal-dot due"></span>到期</span>' +
    '<span class="lg"><span class="cal-dot start"></span>开始</span>' +
    '<span class="lg"><span class="cal-dot lunar"></span>农历提醒</span>' +
    '<span class="lg"><span class="cal-dot repeat"></span>重复实例</span>' +
    '<span class="lg"><span style="color:var(--danger)">节</span>节假日（重复任务自动跳过）</span>';
  content.appendChild(legend);
}

/* 周视图（议程式 7 列） */
async function renderWeekView(content) {
  const ws = mondayOf(S.today, S.weekOffset);
  const we = mondayOf(S.today, S.weekOffset, 6);
  const data = await apiGet('/api/calendar?start=' + ws + '&end=' + we);
  const grid = document.createElement('div');
  grid.className = 'week-grid';
  for (let i = 0; i < 7; ++i) {
    const date = mondayOf(S.today, S.weekOffset, i);
    const day = (data.days || []).find(d => d.date === date);
    if (!day) continue;
    const isToday = day.date === S.today;
    const isWeekend = i >= 5;
    const shown = (day.tasks || []).slice(0, 8);
    const more = (day.tasks || []).length > 8
      ? '<div class="week-task" style="color:var(--text-3)">+' + ((day.tasks || []).length - 8) + ' 更多</div>' : '';
    grid.insertAdjacentHTML('beforeend',
      '<div class="week-col' + (isToday ? ' today' : '') + '" data-act="cal-day" data-date="' + day.date + '">' +
      '<div class="wc-head">' +
      '<div class="wc-dow' + (isWeekend ? '" style="color:var(--danger)"' : '"') + '">周' + CAL_DOW[i] + '</div>' +
      '<div class="wc-day">' + parseInt(day.date.slice(8, 10), 10) + '</div>' +
      '<div class="wc-lunar' + (day.isHoliday ? ' holiday' : '') + '">' +
        esc(day.holidayName || day.lunar || '') + '</div>' +
      (day.term ? '<div class="wc-term">' + esc(day.term) + '</div>' : '') +
      '</div>' +
      '<div class="wc-tasks">' +
      shown.map(e => {
        const t = e.task;
        const cls = e.kind === 'start' ? 'wk-start' : e.kind === 'lunar' ? 'wk-lunar' : '';
        return '<div class="week-task ' + cls + (t.status === 'done' ? ' wk-done' : '') + '" ' +
          'data-act="edit" data-id="' + t.id + '" title="' + esc(t.title) + '">' + esc(t.title) + '</div>';
      }).join('') + more +
      '</div></div>');
  }
  content.appendChild(grid);
}

/* 点击日期 → 当日任务详情弹窗 */
async function openDayModal(dateISO) {
  const [y, m] = dateISO.split('-').map(Number);
  const data = await apiGet('/api/calendar?year=' + y + '&month=' + m);
  const day = (data.days || []).find(d => d.date === dateISO);
  const body = document.createElement('div');
  if (!day || !day.tasks.length) {
    body.innerHTML = '<div class="empty-tip">' + fmtDate(dateISO) + ' 没有任务。</div>';
  } else {
    const rows = day.tasks.map(e => {
      const t = e.task;
      const kindTag = e.kind === 'lunar' ? '🏮农历' : e.kind === 'start' ? '开始' :
        (t.recurringInstance ? '↻重复' : '到期');
      return '<div class="task-row" data-id="' + t.id + '">' +
        '<div class="t-check" data-act="' + (t.status === 'done' ? 'reopen' : 'done') + '" data-id="' + t.id + '">' +
        (t.status === 'done' ? '✓' : '') + '</div>' +
        '<div class="t-main"><div class="t-title' + (t.status === 'done' ? ' si-done' : '') + '">' +
        esc(t.title) + '</div>' +
        '<div class="t-meta"><span class="m-chip">' + kindTag + '</span>' + taskMeta(t) + '</div></div>' +
        '<div class="t-actions"><button class="t-btn" data-act="edit" data-id="' + t.id + '">✎</button></div>' +
        '</div>';
    });
    body.innerHTML = '<div class="list">' + rows.join('') + '</div>';
  }
  openModal('当日任务 · ' + dateISO, body, null, null, false);
}

/* ---------------- 看板视图 ---------------- */
async function renderKanban(content) {
  const head = viewHead('看板', '待办 / 进行中 / 已完成');
  content.appendChild(head);

  const data = await apiGet('/api/kanban');
  const cols = data.columns || {};

  function kbCard(t) {
    const over = t.dueDate && daysBetween(S.today, t.dueDate) < 0 && t.status !== 'done';
    const pCls = t.priority === 2 ? 'p-h' : t.priority === 0 ? 'p-l' : 'p-m';
    const tags = (t.tags || []).slice(0, 3).map(x =>
      '<span class="tag-pill">' + esc(x.name) + '</span>').join('');
    return '<div class="kb-card ' + pCls + (t.status === 'done' ? ' done' : '') +
      '" data-act="edit" data-id="' + t.id + '">' +
      '<div class="kb-title">' + esc(t.title) +
      (t.children && t.children.length ? ' <span style="color:var(--text-3);font-size:11px">(' + t.children.length + ')</span>' : '') +
      '</div>' +
      '<div class="kb-meta">' +
      (t.dueDate ? '<span class="kb-due' + (over ? ' over' : '') + '">' + fmtDate(t.dueDate) + '</span>' : '') +
      (t.lunarRemind ? '<span class="kb-due">🏮' + esc(t.lunarText || '') + '</span>' : '') +
      (t.repeatRule && t.repeatRule.freq ? '<span class="kb-due">↻</span>' : '') +
      (t.blocked ? '<span class="kb-due over">⛔</span>' : '') +
      tags + '</div></div>';
  }

  const kanban = document.createElement('div');
  kanban.className = 'kanban';
  for (const [key, label] of [['todo', '待办'], ['doing', '进行中'], ['done', '已完成']]) {
    const list = cols[key] || [];
    const col = document.createElement('div');
    col.className = 'kb-col ' + key;
    col.innerHTML = '<div class="kb-col-head"><span>' + label + '</span>' +
      '<span class="cnt">' + list.length + '</span></div>' +
      '<div class="kb-list">' + list.map(kbCard).join('') +
      (list.length ? '' : '<div class="empty-tip">空</div>') + '</div>';
    kanban.appendChild(col);
  }
  content.appendChild(kanban);
}

/* ---------------- 筛选视图 ---------------- */
async function renderFilters(content) {
  const data = await apiGet('/api/filters');
  S.filters = data.filters || [];

  const head = viewHead('筛选', '保存常用查询，一键执行');
  content.appendChild(head);

  // 新建筛选表单
  const form = document.createElement('div');
  form.className = 'filter-form';
  form.innerHTML =
    '<div class="ff tf-full"><label>筛选名称</label><input id="f-name" placeholder="例如：学习高优先级"></div>' +
    '<div class="ff"><label>标签</label><select id="f-tag"><option value="">全部</option>' +
    S.tags.map(t => '<option value="' + esc(t.name) + '">' + esc(t.name) + '</option>').join('') + '</select></div>' +
    '<div class="ff"><label>优先级</label><select id="f-prio">' +
    '<option value="">全部</option><option value="2">高</option><option value="1">中</option><option value="0">低</option>' +
    '</select></div>' +
    '<div class="ff"><label>N 天内到期</label><select id="f-within">' +
    '<option value="0">不限</option><option value="3">3 天内</option><option value="7">7 天内</option>' +
    '<option value="14">14 天内</option><option value="30">30 天内</option></select></div>' +
    '<div class="ff"><label>状态</label><select id="f-status">' +
    '<option value="">全部</option><option value="todo">待办</option><option value="doing">进行中</option>' +
    '<option value="done">已完成</option></select></div>' +
    '<div class="ff"><button class="btn btn-primary" data-act="save-filter">保存筛选</button></div>';
  content.appendChild(form);

  // 筛选列表
  const list = document.createElement('div');
  list.className = 'filter-list';
  if (!S.filters.length) {
    list.innerHTML = '<div class="empty-tip" style="background:var(--panel);border:1px solid var(--border);border-radius:10px">尚无保存的筛选。</div>';
  } else {
    list.innerHTML = S.filters.map(f => {
      const spec = f.spec || {};
      const desc = [
        spec.tag ? '标签 ' + esc(spec.tag) : '',
        spec.priority !== undefined && spec.priority !== null && spec.priority !== '' ? '优先级 ' + (PRIO[spec.priority] || {}).label : '',
        spec.dueWithin ? spec.dueWithin + ' 天内到期' : '',
        spec.status ? STATUS_LABEL[spec.status] : ''
      ].filter(Boolean).join(' · ') || '全部任务';
      return '<div class="filter-item">' +
        '<div><div class="f-name">' + esc(f.name) + '</div>' +
        '<div class="f-desc">' + desc + '</div></div>' +
        '<button class="btn btn-sm" data-act="run-filter" data-id="' + f.id + '">执行</button>' +
        '<button class="btn btn-sm btn-danger" data-act="del-filter" data-id="' + f.id + '">删除</button>' +
        '</div>';
    }).join('');
  }
  content.appendChild(list);
}

/* 执行保存的筛选 */
async function runFilter(f) {
  const spec = f.spec || {};
  const qs = new URLSearchParams();
  qs.set('include_children', '1');
  qs.set('parent', '0');
  if (spec.tag) qs.set('tag', spec.tag);
  if (spec.priority !== undefined && spec.priority !== null && spec.priority !== '')
    qs.set('priority', spec.priority);
  if (spec.dueWithin) qs.set('due_within', spec.dueWithin);
  if (spec.status) qs.set('status', spec.status);

  const head = viewHead('筛选结果 · ' + f.name);
  const body = document.createElement('div');
  body.appendChild(head);
  const loading = document.createElement('div');
  loading.className = 'loading';
  loading.textContent = '查询中…';
  body.appendChild(loading);
  openModal('', body, null, true, true);

  try {
    const data = await apiGet('/api/tasks?' + qs.toString());
    loading.remove();
    const card = document.createElement('div');
    card.className = 'card';
    card.innerHTML = taskListHTML(data.tasks, { tree: true, none: '没有符合条件的任务' });
    body.appendChild(card);
  } catch (e) {
    loading.textContent = '查询失败：' + e.message;
  }
}

/* ---------------- 统计仪表盘 ---------------- */
async function renderStats(content) {
  const data = await apiGet('/api/stats');
  const tot = data.totals || {};
  const head = viewHead('统计', '完成趋势 · 连续打卡 · 项目进度');
  content.appendChild(head);

  const cards = document.createElement('div');
  cards.className = 'stat-cards';
  const card = (num, label, cls) =>
    '<div class="stat-card ' + cls + '"><div class="sc-num">' + num + '</div>' +
    '<div class="sc-label">' + label + '</div></div>';
  cards.innerHTML =
    card(tot.todo || 0, '待办', 'c-blue') +
    card(tot.doing || 0, '进行中', 'c-warn') +
    card(tot.done || 0, '已完成', 'c-green') +
    card(tot.overdue || 0, '已逾期', 'c-red') +
    card(tot.dueToday || 0, '今日到期', 'c-warn') +
    card(data.streak || 0, '连续打卡（天）', 'c-purple') +
    card(data.pomodoros || 0, svgIcon('pomodoro', 14) + ' 番茄钟总计', 'c-red');
  content.appendChild(cards);

  // 年度完成热力图（GitHub 风格格子图）
  try {
    const hmYear = S.heatYear || parseInt(S.today.slice(0, 4), 10);
    const hm = await apiGet('/api/heatmap?year=' + hmYear);
    const map = {};
    (hm.days || []).forEach(d => { map[d.date] = d.count; });
    const sec = document.createElement('div');
    sec.className = 'stat-sec';
    sec.innerHTML = '<h3 style="display:flex;align-items:center;gap:10px">' + hmYear + ' 年完成热力图' +
      '<span style="margin-left:auto;display:flex;gap:6px">' +
      '<button class="btn btn-sm" data-act="hm-prev">‹ ' + (hmYear - 1) + '</button>' +
      '<button class="btn btn-sm" data-act="hm-next">' + (hmYear + 1) + ' ›</button></span></h3>';
    const box = document.createElement('div');
    box.className = 'stat-box';
    // 网格：列=周（周一开头），行=周一..周日
    const jan1 = new Date(hmYear, 0, 1);
    const off = (jan1.getDay() + 6) % 7;                 // 1月1日前面补的空白天数
    const start = new Date(hmYear, 0, 1 - off);          // 本年第一个周一
    const colW = 13, rowH = 13;
    const weeks = 53;
    const W = weeks * colW + 4, H = 7 * rowH + 18;
    const todayTs = S.today ? new Date(S.today + 'T23:59:59').getTime() : Date.now();
    let svg = '<svg class="heatmap-svg" viewBox="0 0 ' + W + ' ' + H + '" width="100%" style="max-width:' + W + 'px">';
    let lastMonth = -1;
    for (let wk = 0; wk < weeks; ++wk) {
      for (let d = 0; d < 7; ++d) {
        const cur = new Date(start);
        cur.setDate(start.getDate() + wk * 7 + d);
        if (cur.getFullYear() !== hmYear) continue;
        if (cur.getTime() > todayTs) continue;           // 未来日期不渲染
        const iso = cur.getFullYear() + '-' + String(cur.getMonth() + 1).padStart(2, '0') +
          '-' + String(cur.getDate()).padStart(2, '0');
        const c = map[iso] || 0;
        const lvl = c === 0 ? 0 : c <= 2 ? 1 : c <= 4 ? 2 : c <= 7 ? 3 : 4;
        svg += '<rect class="hm-cell hm-l' + lvl + '" x="' + (wk * colW) + '" y="' + (d * rowH + 16) +
          '" width="11" height="11" rx="2"><title>' + iso + '：完成 ' + c + ' 项</title></rect>';
      }
      const monday = new Date(start);
      monday.setDate(start.getDate() + wk * 7);
      if (monday.getFullYear() === hmYear && monday.getMonth() !== lastMonth) {
        lastMonth = monday.getMonth();
        svg += '<text class="hm-month" x="' + (wk * colW) + '" y="11">' + (lastMonth + 1) + '月</text>';
      }
    }
    svg += '</svg>';
    const lg = '<div class="hm-legend">少' +
      '<span class="lg-cell hm-l0"></span><span class="lg-cell hm-l1"></span>' +
      '<span class="lg-cell hm-l2"></span><span class="lg-cell hm-l3"></span>' +
      '<span class="lg-cell hm-l4"></span>多</div>' +
      '<div class="hm-total">全年完成 <b>' + (hm.total || 0) + '</b> 项 · 单日最高 <b>' + (hm.max || 0) + '</b> 项</div>';
    box.innerHTML = svg + lg;
    sec.appendChild(box);
    content.appendChild(sec);
  } catch (e) { /* 热力图加载失败不阻塞统计视图 */ }

  // 14 天完成趋势（SVG 柱状图）
  const trend = data.trend || [];
  if (trend.length) {
    const sec = document.createElement('div');
    sec.className = 'stat-sec';
    sec.innerHTML = '<h3>最近 14 天完成趋势</h3>';
    const maxC = Math.max(1, ...trend.map(t => t.count));
    const W = 860, H = 150, padL = 28, padB = 22, padT = 10;
    const bw = (W - padL - 8) / trend.length;
    let svg = '<svg class="trend-chart" viewBox="0 0 ' + W + ' ' + H + '">';
    for (let i = 0; i <= maxC; ++i) {
      const y = padT + (H - padT - padB) * (1 - i / maxC);
      svg += '<line x1="' + padL + '" y1="' + y + '" x2="' + W + '" y2="' + y +
        '" stroke="var(--border)" stroke-width="1"/>' +
        '<text x="' + (padL - 6) + '" y="' + (y + 3) + '" text-anchor="end">' + i + '</text>';
    }
    trend.forEach((t, i) => {
      const x = padL + i * bw + bw * 0.18;
      const h = (H - padT - padB) * (t.count / maxC);
      const y = H - padB - h;
      const isToday = i === trend.length - 1;
      svg += '<rect class="tc-bar' + (isToday ? ' today' : '') + '" x="' + x + '" y="' + y +
        '" width="' + (bw * 0.64) + '" height="' + Math.max(h, t.count ? 2 : 0) + '" rx="2">' +
        '<title>' + t.date + ' 完成 ' + t.count + ' 项</title></rect>';
      svg += '<text x="' + (x + bw * 0.32) + '" y="' + (H - padB + 13) + '" text-anchor="middle">' +
        parseInt(t.date.slice(8, 10), 10) + '</text>';
    });
    svg += '</svg>';
    const box = document.createElement('div');
    box.className = 'stat-box';
    box.innerHTML = svg;
    sec.appendChild(box);
    content.appendChild(sec);
  }

  // 项目进度
  const projects = data.projects || [];
  if (projects.length) {
    const sec = document.createElement('div');
    sec.className = 'stat-sec';
    sec.innerHTML = '<h3>项目进度</h3>';
    const box = document.createElement('div');
    box.className = 'stat-box';
    box.innerHTML = projects.map(p => {
      const total = (p.open || 0) + (p.done || 0);
      const pct = total ? Math.round((p.done || 0) * 100 / total) : 0;
      return '<div class="proj-progress">' +
        '<div class="pp-head"><span>' + esc(p.name) + '</span>' +
        '<span style="color:var(--text-3)">' + (p.done || 0) + '/' + total + '（' + pct + '%）</span></div>' +
        '<div class="pp-track"><div class="pp-fill" style="width:' + pct + '%;background:' +
        esc(p.color || 'var(--primary)') + '"></div></div></div>';
    }).join('');
    sec.appendChild(box);
    content.appendChild(sec);
  }

  // 优先级分布
  const prio = data.priorityDist || [];
  if (prio.length) {
    const sec = document.createElement('div');
    sec.className = 'stat-sec';
    sec.innerHTML = '<h3>未完成优先级分布</h3>';
    const box = document.createElement('div');
    box.className = 'stat-box';
    const maxP = Math.max(1, ...prio.map(p => p.count));
    box.innerHTML = prio.map(p => {
      const meta = PRIO[p.priority] || { label: '中' };
      const color = p.priority === 2 ? 'var(--danger)' : p.priority === 0 ? 'var(--text-3)' : 'var(--warn)';
      return '<div class="proj-progress"><div class="pp-head"><span>' + meta.label + '优先级</span>' +
        '<span style="color:var(--text-3)">' + p.count + ' 项</span></div>' +
        '<div class="pp-track"><div class="pp-fill" style="width:' + (p.count * 100 / maxP) +
        '%;background:' + color + '"></div></div></div>';
    }).join('');
    sec.appendChild(box);
    content.appendChild(sec);
  }
}

/* ---------------- 甘特 / 依赖图 ---------------- */
async function renderGantt(content) {
  const head = viewHead('甘特图', '任务时间条 + 依赖箭头（基于开始/截止日期）');
  content.appendChild(head);

  const data = await apiGet('/api/tree');
  const flat = [];
  (function walk(list, depth) {
    for (const t of list || []) {
      flat.push(Object.assign({ depth: depth }, t));
      if (t.children) walk(t.children, depth + 1);
    }
  })(data.tree || [], 0);

  const rows = flat.filter(t => t.startDate || t.dueDate);
  if (!rows.length) {
    content.insertAdjacentHTML('beforeend',
      '<div class="empty-tip" style="background:var(--panel);border:1px solid var(--border);border-radius:10px">' +
      '暂无带日期的任务。给任务设置「开始 / 截止日期」后即可在此查看时间线。</div>');
    return;
  }

  // 时间范围
  const allDates = [];
  rows.forEach(t => {
    if (t.startDate) allDates.push(t.startDate);
    if (t.dueDate) allDates.push(t.dueDate);
  });
  const minD = allDates.slice().sort()[0];
  const maxD = allDates.slice().sort().pop();
  const totalDays = Math.max(1, daysBetween(minD, maxD) + 1);
  const labelW = 230, rowH = 30, headH = 46;
  const dayW = Math.max(22, Math.min(64, 900 / totalDays));
  const W = labelW + totalDays * dayW + 12;
  const H = headH + rows.length * rowH + 10;

  const xOf = iso => labelW + daysBetween(minD, iso) * dayW;
  let svg = '<svg class="gantt-svg" width="' + W + '" height="' + H + '" viewBox="0 0 ' + W + ' ' + H + '">' +
    '<defs><marker id="garrow" viewBox="0 0 8 8" refX="7" refY="4" markerWidth="7" markerHeight="7" ' +
    'orient="auto"><path d="M0,0 L8,4 L0,8 z" fill="var(--text-3)"/></marker></defs>';

  // 日期网格与表头
  for (let i = 0; i < totalDays; ++i) {
    const iso = (() => {
      const [y, m, d] = minD.split('-').map(Number);
      const dt = new Date(y, m - 1, d + i);
      const p = n => String(n).padStart(2, '0');
      return dt.getFullYear() + '-' + p(dt.getMonth() + 1) + '-' + p(dt.getDate());
    })();
    const dow = new Date(iso + 'T00:00').getDay();
    const x = labelW + i * dayW;
    if (dow === 0 || dow === 6)
      svg += '<rect class="g-grid g-weekend" x="' + x + '" y="' + headH + '" width="' + dayW +
        '" height="' + (H - headH - 10) + '"/>';
    if (i === 0 || dow === 1 || totalDays <= 31) {
      svg += '<text class="g-head" x="' + (x + dayW / 2) + '" y="' + (headH - 18) +
        '" text-anchor="middle">' + parseInt(iso.slice(5, 7), 10) + '/' + parseInt(iso.slice(8, 10), 10) + '</text>';
      svg += '<text class="g-head" x="' + (x + dayW / 2) + '" y="' + (headH - 6) +
        '" text-anchor="middle">周' + CAL_DOW[(dow + 6) % 7] + '</text>';
    }
    svg += '<line class="g-grid" x1="' + x + '" y1="' + headH + '" x2="' + x + '" y2="' + (H - 10) + '"/>';
    if (iso === S.today)
      svg += '<line class="g-today-line" x1="' + (x + dayW / 2) + '" y1="' + headH +
        '" x2="' + (x + dayW / 2) + '" y2="' + (H - 10) + '"/>';
  }
  svg += '<line class="g-grid" x1="' + (labelW + totalDays * dayW) + '" y1="' + headH +
    '" x2="' + (labelW + totalDays * dayW) + '" y2="' + (H - 10) + '"/>';
  svg += '<line class="g-grid" x1="0" y1="' + headH + '" x2="' + W + '" y2="' + headH + '"/>';

  // 行：背景 / 标签 / 时间条
  const barGeom = {};
  rows.forEach((t, i) => {
    const y = headH + i * rowH;
    const s = t.startDate || t.dueDate;
    const e = t.dueDate || t.startDate;
    const x1 = xOf(s);
    const x2 = xOf(e) + dayW;
    barGeom[t.id] = { x1: x1, x2: x2, y: y + rowH / 2 };
    let cls = 'g-bar';
    if (t.status === 'done') cls += ' g-done';
    else if (t.status === 'doing') cls += ' g-doing';
    else if (t.dueDate && daysBetween(S.today, t.dueDate) < 0) cls += ' g-over';
    const indent = 10 + (t.depth || 0) * 14;
    svg += '<g class="g-row">' +
      '<rect class="g-rowbg" data-act="edit" data-id="' + t.id + '" x="0" y="' + y +
      '" width="' + W + '" height="' + rowH + '" fill="transparent" style="cursor:pointer"/>' +
      '<text class="g-label' + (t.status === 'done' ? ' g-done' : '') + '" x="' + indent +
      '" y="' + (y + rowH / 2 + 4) + '">' + esc(t.title.slice(0, 26)) + '</text>' +
      '<rect class="' + cls + '" data-act="edit" data-id="' + t.id + '" x="' + x1 + '" y="' + (y + 7) +
      '" width="' + Math.max(x2 - x1, 6) + '" height="' + (rowH - 14) + '" rx="4" style="cursor:pointer">' +
      '<title>' + esc(t.title) + '\n' + s + ' → ' + e + '</title></rect>' +
      '</g>';
  });

  // 依赖箭头（前置完成 → 本任务开始）
  rows.forEach(t => {
    for (const d of t.dependsOn || []) {
      const g = barGeom[d.id];
      if (!g) continue;
      const me = barGeom[t.id];
      const from = { x: g.x2, y: g.y };
      const to = { x: me.x1, y: me.y };
      if (to.x > from.x) {
        const midX = from.x + (to.x - from.x) / 2;
        svg += '<path class="g-dep" d="M' + from.x + ',' + from.y + ' C' + midX + ',' + from.y +
          ' ' + midX + ',' + to.y + ' ' + (to.x - 2) + ',' + to.y + '"/>';
      } else {
        const lift = from.y + (to.y > from.y ? 1 : -1) * rowH * 0.7;
        svg += '<path class="g-dep" d="M' + from.x + ',' + from.y + ' C' + (from.x + 14) + ',' + lift +
          ' ' + (to.x - 14) + ',' + lift + ' ' + (to.x - 2) + ',' + to.y + '"/>';
      }
    }
  });
  svg += '</svg>';

  const wrap = document.createElement('div');
  wrap.className = 'gantt-wrap';
  wrap.innerHTML = svg;
  content.appendChild(wrap);

  const legend = document.createElement('div');
  legend.className = 'cal-legend';
  legend.innerHTML =
    '<span class="lg"><span style="width:14px;height:8px;background:var(--primary);border-radius:3px;display:inline-block"></span>待办</span>' +
    '<span class="lg"><span style="width:14px;height:8px;background:var(--warn);border-radius:3px;display:inline-block"></span>进行中</span>' +
    '<span class="lg"><span style="width:14px;height:8px;background:var(--ok);border-radius:3px;display:inline-block"></span>已完成</span>' +
    '<span class="lg"><span style="width:14px;height:8px;background:var(--danger);border-radius:3px;display:inline-block"></span>已逾期</span>' +
    '<span class="lg">→ 依赖（前置完成后才能开始）</span>';
  content.appendChild(legend);
}

/* ---------------- 回收站 ---------------- */
async function renderTrash(content) {
  const data = await apiGet('/api/trash');
  const head = viewHead('回收站',
    '删除的任务保留 30 天，可恢复或彻底删除',
    '<div class="spacer" style="flex:1"></div>' +
    '<button class="btn btn-sm btn-danger" data-act="trash-clear">清空回收站</button>');
  content.appendChild(head);

  const card = document.createElement('div');
  card.className = 'card';
  const list = data.tasks || [];
  card.innerHTML = list.length
    ? list.map(t =>
      '<div class="trash-row" data-id="' + t.id + '">' +
      '<div class="tr-title' + (t.status === 'done' ? ' si-done' : '') + '">' + esc(t.title) + '</div>' +
      '<span class="tr-when">删除于 ' + esc((t.deletedAt || '').slice(0, 16).replace('T', ' ')) + '</span>' +
      '<button class="btn btn-sm" data-act="trash-restore" data-id="' + t.id + '">恢复</button>' +
      '<button class="btn btn-sm btn-danger" data-act="trash-purge" data-id="' + t.id + '">彻底删除</button>' +
      '</div>').join('')
    : '<div class="empty-tip">回收站是空的</div>';
  content.appendChild(card);
}

/* ---------------- 导出弹窗 ---------------- */
function openExportModal() {
  const body = document.createElement('div');
  body.className = 'export-menu';
  body.innerHTML =
    ['todotxt|☑|Todo.txt|通用纯文本格式，每行一个任务，可被多数工具导入',
     'json|{}|JSON|完整结构化数据，含项目/标签/依赖/重复规则，适合备份',
     'csv|▤|CSV|表格格式，可用 Excel / Numbers 打开',
     'backup|◈|完整备份快照|全量快照（任务+项目+标签+模板+筛选+节假日），可在另一台设备「导入 → 同步合并」']
    .map(s => {
      const [fmt, ico, name, desc] = s.split('|');
      return '<div class="export-item" data-act="export-go" data-fmt="' + fmt + '">' +
        '<span class="ei-ico">' + ico + '</span>' +
        '<div><div class="ei-name">' + name + '</div><div class="ei-desc">' + desc + '</div></div></div>';
    }).join('') +
    '<div class="export-item" data-act="sync-open">' +
    '<span class="ei-ico">' + svgIcon('sync', 16) + '</span>' +
    '<div><div class="ei-name">同步合并备份</div><div class="ei-desc">把另一台设备导出的备份快照合并进本机（不删除本地数据）</div></div></div>';
  openModal('导出 / 同步', body, null);
}

/* ---------------- 快捷键帮助 ---------------- */
function openHelpModal() {
  const body = document.createElement('div');
  body.innerHTML = '<table class="kbd-table">' +
    [['<kbd>⌘K</kbd> / <kbd>Ctrl K</kbd>', '打开命令面板'],
     ['<kbd>⌘Z</kbd> / <kbd>Ctrl Z</kbd>', '撤销最近一次操作（创建/更新/删除/完成等均可撤销）'],
     ['<kbd>N</kbd>', '新建任务'],
     ['<kbd>Q</kbd>', '聚焦快速录入框（自然语言）'],
     ['<kbd>/</kbd>', '聚焦搜索框'],
     ['<kbd>P</kbd>', '打开 / 收起番茄钟'],
     ['<kbd>F</kbd>', '进入专注模式（全屏番茄钟，Esc 退出）'],
     ['<kbd>D</kbd>', '切换到时间块日视图'],
     ['<kbd>T</kbd>', '切换深色 / 浅色主题'],
     ['<kbd>1</kbd> … <kbd>9</kbd>', '切换视图（今日/项目/标签/日历/看板/筛选/统计/甘特/回收站）'],
     ['<kbd>?</kbd>', '显示本帮助'],
     ['<kbd>Esc</kbd>', '关闭弹窗 / 面板 / 专注模式']]
    .map(r => '<tr><td>' + r[0] + '</td><td>' + r[1] + '</td></tr>').join('') +
    '</table>' +
    '<div style="margin-top:12px;font-size:12px;color:var(--text-3)">' +
    '快速录入语法：顶栏输入「明天下午3点 写周报 #工作 !高 /项目名」后回车，自动解析日期/时间/标签/优先级/项目；' +
    '常用任务可在编辑器里「存为模板」，命令面板中一键实例化。' +
    '今日视图支持：批量模式（勾选后批量完成/移动/打标签）、拖拽任务行手动排序；' +
    '任务行 ' + svgIcon('pomodoro', 12) + ' 按钮可将任务加入番茄钟，' + svgIcon('focus', 12) + ' 按钮进入全屏专注。' +
    '日视图点击时间轴空白处可在该时刻新建任务；多台设备间用「导出→完整备份快照 / 同步合并」搬数据。</div>';
  openModal('键盘快捷键', body, null);
}

/* ---------------- 命令面板（⌘K） ---------------- */
const PALETTE_CMDS = [
  { ico: svgIcon('today'), label: '前往：今日', hint: '1', run: () => switchView('today') },
  { ico: svgIcon('projects'), label: '前往：项目', hint: '2', run: () => switchView('projects') },
  { ico: svgIcon('tags'), label: '前往：标签', hint: '3', run: () => switchView('tags') },
  { ico: svgIcon('calendar'), label: '前往：日历', hint: '4', run: () => switchView('calendar') },
  { ico: svgIcon('kanban'), label: '前往：看板', hint: '5', run: () => switchView('kanban') },
  { ico: svgIcon('filters'), label: '前往：筛选', hint: '6', run: () => switchView('filters') },
  { ico: svgIcon('stats'), label: '前往：统计', hint: '7', run: () => switchView('stats') },
  { ico: svgIcon('gantt'), label: '前往：甘特图', hint: '8', run: () => switchView('gantt') },
  { ico: svgIcon('trash'), label: '前往：回收站', hint: '9', run: () => switchView('trash') },
  { ico: svgIcon('day'), label: '前往：日视图（时间块）', hint: 'D', run: () => switchView('day') },
  { ico: svgIcon('undo'), label: '撤销最近一次操作', hint: '⌘Z', run: () => undoLast() },
  { ico: svgIcon('focus'), label: '进入专注模式（全屏番茄钟）', hint: 'F', run: () => enterFocusMode() },
  { ico: svgIcon('sync'), label: '多端同步（合并备份快照）', hint: '', run: () => openSyncModal() },
  { ico: svgIcon('plus'), label: '新建任务', hint: 'N', run: () => openTaskEditor(null) },
  { ico: svgIcon('bolt'), label: '从模板新建任务', hint: '', run: () => openTemplatesModal() },
  { ico: svgIcon('bolt'), label: '快速录入（自然语言）', hint: 'Q', run: () => { const q = $('#quick-input'); if (q) q.focus(); } },
  { ico: svgIcon('pomodoro'), label: '打开番茄钟', hint: 'P', run: () => showPomodoro() },
  { ico: svgIcon('theme'), label: '切换深色 / 浅色主题', hint: 'T', run: () => toggleTheme() },
  { ico: svgIcon('export'), label: '导出数据', hint: '', run: () => openExportModal() },
  { ico: svgIcon('calendar'), label: '生成本年法定节假日', hint: '', run: async () => {
      try {
        const r = await api('POST', '/api/holidays/auto?year=' + S.today.slice(0, 4));
        toast('已生成 ' + r.year + ' 年 ' + r.added + ' 天节假日', 'ok');
      } catch (e) { toast(e.message, 'err'); }
    } },
  { ico: svgIcon('export'), label: '立即备份数据库', hint: '', run: async () => {
      try {
        await api('POST', '/api/backups');
        toast('备份完成（数据库目录 /backups）', 'ok');
      } catch (e) { toast(e.message, 'err'); }
    } },
  { ico: svgIcon('filters'), label: '键盘快捷键帮助', hint: '?', run: () => openHelpModal() }
];

let paletteState = { items: [], sel: 0 };

function openPalette() {
  closePalette();
  const mask = document.createElement('div');
  mask.className = 'palette-mask';
  mask.id = 'palette-root';
  mask.innerHTML =
    '<div class="palette">' +
    '<input class="palette-input" id="palette-input" placeholder="输入命令、或搜索任务…" autocomplete="off">' +
    '<div class="palette-list" id="palette-list"></div></div>';
  document.body.appendChild(mask);
  mask.addEventListener('click', e => { if (e.target === mask) closePalette(); });
  const input = $('#palette-input');
  input.focus();
  input.addEventListener('input', () => fillPalette(input.value.trim()));
  input.addEventListener('keydown', async e => {
    if (e.key === 'Escape') { closePalette(); }
    else if (e.key === 'ArrowDown') { e.preventDefault(); movePaletteSel(1); }
    else if (e.key === 'ArrowUp') { e.preventDefault(); movePaletteSel(-1); }
    else if (e.key === 'Enter') { e.preventDefault(); await runPaletteSel(); }
  });
  fillPalette('');
}

function closePalette() {
  var p = $('#palette-root');
  if (!p) return;
  if (!p.classList.contains('closing')) {
    p.classList.add('closing');
    setTimeout(function() { if (p.parentNode) p.remove(); }, 160);
  }
}

async function fillPalette(q) {
  const list = $('#palette-list');
  if (!list) return;
  let items = PALETTE_CMDS.filter(c => !q || c.label.toLowerCase().indexOf(q.toLowerCase()) >= 0)
    .map(c => ({ type: 'cmd', cmd: c }));
  // 空查询时只显示命令；有查询时附加任务结果
  if (q) {
    try {
      const r = await apiGet('/api/search?q=' + encodeURIComponent(q));
      for (const t of (r.tasks || []).slice(0, 8)) {
        items.push({ type: 'task', task: t });
      }
    } catch (e) { /* 忽略 */ }
  }
  paletteState.items = items;
  paletteState.sel = 0;
  renderPaletteList();
}

function renderPaletteList() {
  const list = $('#palette-list');
  if (!list) return;
  let html = '<div class="pal-group">命令</div>';
  let lastType = 'cmd';
  paletteState.items.forEach((it, i) => {
    if (it.type !== lastType) { html += '<div class="pal-group">任务</div>'; lastType = it.type; }
    const sel = i === paletteState.sel ? ' sel' : '';
    if (it.type === 'cmd') {
      html += '<div class="pal-item' + sel + '" data-pi="' + i + '">' +
        '<span class="pal-ico">' + it.cmd.ico + '</span>' +
        '<span class="pal-label">' + esc(it.cmd.label) + '</span>' +
        (it.cmd.hint ? '<span class="pal-hint">' + it.cmd.hint + '</span>' : '') + '</div>';
    } else {
      const t = it.task;
      html += '<div class="pal-item' + sel + '" data-pi="' + i + '">' +
        '<span class="pal-ico">☑</span>' +
        '<span class="pal-label">' + esc(t.title) + '</span>' +
        '<span class="pal-hint">' + (t.dueDate ? fmtDate(t.dueDate) : '无日期') + '</span></div>';
    }
  });
  list.innerHTML = html;
  list.querySelectorAll('.pal-item').forEach(el => {
    el.addEventListener('click', () => {
      paletteState.sel = parseInt(el.dataset.pi, 10);
      runPaletteSel();
    });
  });
}

function movePaletteSel(d) {
  const n = paletteState.items.length;
  if (!n) return;
  paletteState.sel = (paletteState.sel + d + n) % n;
  renderPaletteList();
  const cur = $('#palette-list .pal-item.sel');
  if (cur) cur.scrollIntoView({ block: 'nearest' });
}

async function runPaletteSel() {
  const it = paletteState.items[paletteState.sel];
  if (!it) return;
  closePalette();
  if (it.type === 'cmd') {
    it.cmd.run();
  } else {
    let t = S.taskIndex.get(it.task.id);
    if (!t) {
      try {
        t = await apiGet('/api/tasks/' + it.task.id);
        S.taskIndex.set(t.id, t);
      } catch (e) { toast(e.message, 'err'); return; }
    }
    openTaskEditor(t);
  }
}

/* ---------------- 快速录入（自然语言） ---------------- */
async function quickAdd(text) {
  try {
    const r = await api('POST', '/api/quick-add', { text: text });
    const p = r.parsed || {};
    const bits = [];
    if (p.dueDate) bits.push(fmtDate(p.dueDate));
    if (p.remindTime) bits.push(p.remindTime);
    if (p.priority === 2) bits.push('高优先级');
    if (p.priority === 0) bits.push('低优先级');
    if ((p.tags || []).length) bits.push('#' + p.tags.join(' #'));
    if (p.project && !p.projectMatched) bits.push('项目「' + p.project + '」未找到，已忽略');
    toast('已创建「' + p.title + '」' + (bits.length ? '（' + bits.join(' · ') + '）' : ''), 'ok');
    await refreshAll();
  } catch (e) { toast('快速录入失败：' + e.message, 'err'); }
}

/* ---------------- 任务模板 ---------------- */
function openSaveTemplateModal(payload) {
  const body = document.createElement('div');
  body.innerHTML =
    '<div class="form-row"><label>模板名称</label>' +
    '<input id="tpl-name" class="st-input" type="text" value="' + esc(payload.title || '') + '"></div>' +
    '<label style="display:flex;gap:8px;align-items:center;margin:12px 0;font-size:13px;cursor:pointer">' +
    '<input type="checkbox" id="tpl-offset" checked style="accent-color:var(--primary)">' +
    '日期按「相对今天的天数」保存（每次使用模板时自动换算为新日期）</label>' +
    '<div style="font-size:12px;color:var(--text-3)">模板保存标题、备注、优先级、标签、重复规则与日期等信息。</div>';

  const foot = document.createElement('div');
  foot.style.cssText = 'display:flex;gap:8px;justify-content:flex-end;width:100%';
  const btnCancel = document.createElement('button');
  btnCancel.className = 'btn';
  btnCancel.textContent = '取消';
  btnCancel.addEventListener('click', closeModal);
  const btnSave = document.createElement('button');
  btnSave.className = 'btn btn-primary';
  btnSave.textContent = '保存模板';
  btnSave.addEventListener('click', async () => {
    const name = $('#tpl-name').value.trim() || payload.title;
    if (!name) { toast('请填写模板名称', 'err'); return; }
    const body2 = JSON.parse(JSON.stringify(payload));
    if ($('#tpl-offset').checked) {
      if (body2.dueDate) { body2.dueOffsetDays = daysBetween(S.today, body2.dueDate); body2.dueDate = ''; }
      if (body2.startDate) { body2.startOffsetDays = daysBetween(S.today, body2.startDate); body2.startDate = ''; }
    }
    var restore = btnLoading(btnSave);
    try {
      await api('POST', '/api/templates', { name: name, body: body2 });
      toast('模板已保存', 'ok');
      closeModal();
    } catch (e) { toast(e.message, 'err'); }
    finally { restore(); }
  });
  foot.appendChild(btnCancel);
  foot.appendChild(btnSave);
  openModal('保存为模板', body, foot);
}

async function openTemplatesModal() {
  let list = [];
  try { list = (await apiGet('/api/templates')).templates || []; }
  catch (e) { toast(e.message, 'err'); return; }
  const body = document.createElement('div');
  body.innerHTML = list.length
    ? '<div class="tpl-list">' + list.map(t => {
        const b = t.body || {};
        const bits = [];
        if (b.dueOffsetDays !== undefined) {
          bits.push(b.dueOffsetDays === 0 ? '当天到期'
            : (b.dueOffsetDays > 0 ? '当天 +' + b.dueOffsetDays + ' 天' : '当天 ' + b.dueOffsetDays + ' 天'));
        }
        if (b.priority === 2) bits.push('高优先级');
        if ((b.tags || []).length) bits.push('#' + b.tags.join(' #'));
        if (b.repeatRule && b.repeatRule.freq) bits.push('重复');
        return '<div class="tpl-item">' +
          '<div class="tpl-info">' +
          '<div class="tpl-name">' + esc(t.name) + '</div>' +
          '<div class="tpl-meta">' + esc(b.title || '') + (bits.length ? ' · ' + bits.join(' · ') : '') + '</div>' +
          '</div>' +
          '<button class="btn btn-sm btn-primary" data-tpl-use="' + t.id + '">创建任务</button>' +
          '<button class="btn btn-sm" data-tpl-del="' + t.id + '">删除</button>' +
          '</div>';
      }).join('') + '</div>'
    : '<div class="empty-tip">暂无模板。在任务编辑器里点「存为模板」即可把常用任务存为模板。</div>';
  body.addEventListener('click', async e => {
    const u = e.target.closest('[data-tpl-use]');
    const d = e.target.closest('[data-tpl-del]');
    if (u) {
      try {
        const r = await api('POST', '/api/templates/' + u.dataset.tplUse + '/apply');
        toast('已从模板创建任务 #' + r.task.id, 'ok');
        closeModal();
        await refreshAll();
      } catch (err) { toast(err.message, 'err'); }
    } else if (d) {
      if (!confirm('删除该模板？')) return;
      try {
        await api('DELETE', '/api/templates/' + d.dataset.tplDel);
        toast('模板已删除');
        closeModal();
        openTemplatesModal();
      } catch (err) { toast(err.message, 'err'); }
    }
  });
  openModal('任务模板', body, null);
}

/* ---------------- 番茄钟 ---------------- */
const POMO = { total: 25 * 60, remain: 25 * 60, running: false, taskId: 0, timer: null };

function showPomodoro(taskId) {
  if (taskId) POMO.taskId = taskId;
  $('#pomo-widget').classList.remove('hidden');
  fillPomoTasks();
  renderPomo();
}

async function fillPomoTasks() {
  const sel = $('#pomo-task-select');
  if (!sel) return;
  try {
    const data = await apiGet('/api/tree');
    S.taskIndex.clear();
    buildTaskIndex(data.tree || [], S.taskIndex);
    const open = Array.from(S.taskIndex.values())
      .filter(t => t.status !== 'done' && t.status !== 'archived');
    sel.innerHTML = '<option value="0">（不关联任务）</option>' +
      open.map(t => '<option value="' + t.id + '"' +
        (POMO.taskId === t.id ? ' selected' : '') + '>#' + t.id + ' ' +
        esc(t.title.slice(0, 18)) + '</option>').join('');
  } catch (e) { /* 忽略 */ }
}

function renderPomo() {
  const m = Math.floor(POMO.remain / 60);
  const s = POMO.remain % 60;
  $('#pomo-timer').textContent =
    String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
  $('#pomo-widget').classList.toggle('running', POMO.running);
  const btn = $('#pomo-start');
  btn.textContent = POMO.running ? '暂停' : (POMO.remain < POMO.total ? '继续' : '开始');
}

async function pomoTick() {
  if (!POMO.running) return;
  POMO.remain--;
  if (POMO.remain <= 0) {
    POMO.remain = 0;
    POMO.running = false;
    clearInterval(POMO.timer);
    POMO.timer = null;
    toast('🍅 番茄钟完成！休息 5 分钟吧', 'ok');
    if (POMO.taskId) {
      try {
        const r = await api('POST', '/api/tasks/' + POMO.taskId + '/pomodoro');
        toast('任务已累计 ' + r.pomodoros + ' 个番茄', 'ok');
        await refreshAll();
      } catch (e) { /* 忽略 */ }
    }
  }
  renderPomo();
}

function pomoToggle() {
  POMO.running = !POMO.running;
  if (POMO.running && !POMO.timer)
    POMO.timer = setInterval(pomoTick, 1000);
  if (!POMO.running && POMO.timer) {
    clearInterval(POMO.timer);
    POMO.timer = null;
  }
  renderPomo();
}

function bindPomodoro() {
  $('#pomo-close').addEventListener('click', () => {
    $('#pomo-widget').classList.add('hidden');
  });
  $('#pomo-start').addEventListener('click', pomoToggle);
  $('#pomo-reset').addEventListener('click', () => {
    POMO.running = false;
    if (POMO.timer) { clearInterval(POMO.timer); POMO.timer = null; }
    POMO.remain = POMO.total;
    renderPomo();
  });
  $('#pomo-task-select').addEventListener('change', e => {
    POMO.taskId = parseInt(e.target.value, 10) || 0;
  });
}

/* ---------------- 撤销系统（Ctrl+Z） ---------------- */
async function undoLast() {
  try {
    const info = await apiGet('/api/undo');
    if (!info.canUndo) { toast('没有可撤销的操作'); return; }
    const r = await api('POST', '/api/undo');
    toast(r.desc || '已撤销', 'ok');
    await refreshAll();
  } catch (e) { toast(e.message, 'err'); }
}

/* ---------------- 时间块日视图 ---------------- */
const DAY_HOUR_PX = 44;
let dayNowTimer = null;

function isoAddDays(iso, n) {
  const [y, m, d] = iso.split('-').map(Number);
  const t = new Date(Date.UTC(y, m - 1, d));
  t.setUTCDate(t.getUTCDate() + n);
  return t.toISOString().slice(0, 10);
}

async function renderDay(content) {
  const date = S.dayDate || S.today;
  const data = await apiGet('/api/day?date=' + date);
  const isToday = date === S.today;
  const WD = ['', '周一', '周二', '周三', '周四', '周五', '周六', '周日'];

  const head = viewHead('日视图', '时间块 + 全天 + 已完成');
  content.appendChild(head);

  // 日期导航条
  const nav = document.createElement('div');
  nav.className = 'day-nav';
  nav.innerHTML =
    '<button class="btn btn-sm" data-act="day-prev">‹ 前一天</button>' +
    '<div class="day-title">' +
      '<span class="dt-main">' + fmtDate(date) + ' ' + WD[data.weekday || 1] + '</span>' +
      '<span class="dt-sub">农历' + esc(data.lunarText || '') +
        (data.term ? ' · ' + esc(data.term) : '') +
        (data.holidayName ? ' · 🎉 ' + esc(data.holidayName) : '') + '</span>' +
    '</div>' +
    '<button class="btn btn-sm" data-act="day-today">今天</button>' +
    '<button class="btn btn-sm" data-act="day-next">后一天 ›</button>';
  content.appendChild(nav);

  const layout = document.createElement('div');
  layout.className = 'day-layout';

  // ---- 左侧：0-24 时间轴 ----
  const timeline = document.createElement('div');
  timeline.className = 'day-timeline';
  let labelsHTML = '';
  let slotsHTML = '';
  for (let h = 0; h < 24; ++h) {
    labelsHTML += '<div class="dt-hlabel"><span>' + String(h).padStart(2, '0') + ':00</span></div>';
    slotsHTML += '<div class="dt-slot"></div>';
  }
  timeline.innerHTML =
    '<div class="dt-body">' +
    '<div class="dt-labels">' + labelsHTML + '</div>' +
    '<div class="dt-canvas" id="dt-canvas">' + slotsHTML + '</div>' +
    '</div>';

  const canvas = timeline.querySelector('.dt-canvas');

  // 时间块条目（同时段自动错位排布）
  const byTime = {};
  for (const e of (data.timed || [])) {
    const t = (e.remindTime || '00:00').slice(0, 5);
    (byTime[t] = byTime[t] || []).push(e);
  }
  for (const [time, entries] of Object.entries(byTime)) {
    const [hh, mm] = time.split(':').map(Number);
    const top = (hh * 60 + mm) / 60 * DAY_HOUR_PX;
    entries.forEach((e, i) => {
      const p = PRIO[e.priority] || PRIO[1];
      const el = document.createElement('div');
      el.className = 'dt-entry prio-' + e.priority + (e.virtual ? ' virtual' : '');
      el.style.top = top + 'px';
      el.style.left = (i * 10) + 'px';
      el.style.right = (entries.length > 1 ? (entries.length - 1 - i) * 10 : 0) + 'px';
      el.title = (e.virtual ? '（重复/农历实例）' : '') + time + ' ' + e.title;
      el.innerHTML = '<span class="dte-time">' + time + '</span>' +
        '<span class="dte-title">' + esc(e.title) + '</span>' +
        (e.virtual ? '<span class="dte-flag">' + (e.lunarInstance ? '🏮' : '↻') + '</span>' : '') +
        '<span class="dte-prio ' + p.cls + '">' + p.label + '</span>';
      el.dataset.act = 'edit';
      el.dataset.id = e.id;
      canvas.appendChild(el);
    });
  }

  // 现在时刻红线
  if (isToday) {
    const now = new Date();
    const line = document.createElement('div');
    line.className = 'dt-nowline';
    line.id = 'dt-nowline';
    const upd = () => {
      const n = new Date();
      line.style.top = ((n.getHours() * 60 + n.getMinutes()) / 60 * DAY_HOUR_PX) + 'px';
      line.title = '现在 ' + String(n.getHours()).padStart(2, '0') + ':' + String(n.getMinutes()).padStart(2, '0');
    };
    upd();
    canvas.appendChild(line);
    clearInterval(dayNowTimer);
    dayNowTimer = setInterval(() => {
      if (!$('#dt-nowline')) { clearInterval(dayNowTimer); return; }
      upd();
    }, 60000);
  }

  // 点击空白时段 → 新建任务（预填日期与时间）
  canvas.addEventListener('click', e => {
    if (e.target.closest('.dt-entry')) return;
    const rect = canvas.getBoundingClientRect();
    const y = e.clientY - rect.top;
    const mins = Math.max(0, Math.min(23 * 60 + 45, Math.round(y / DAY_HOUR_PX * 60 / 15) * 15));
    const hh = String(Math.floor(mins / 60)).padStart(2, '0');
    const mm = String(mins % 60).padStart(2, '0');
    openTaskEditor({ dueDate: date, remindTime: hh + ':' + mm }, true);
  });

  layout.appendChild(timeline);

  // ---- 右侧：全天 / 已完成 ----
  const side = document.createElement('div');
  side.className = 'day-side';

  function sideCard(title, items, cls, emptyTip) {
    const rows = (items || []).map(e => {
      const p = PRIO[e.priority] || PRIO[1];
      return '<div class="ds-item" data-act="edit" data-id="' + e.id + '">' +
        '<span class="ds-prio ' + p.cls + '">' + p.label + '</span>' +
        '<span class="ds-title">' + esc(e.title) + '</span>' +
        (e.virtual ? '<span class="ds-flag">' + (e.lunarInstance ? '🏮' : '↻') + '</span>' : '') +
        (e.projectName ? '<span class="ds-proj" style="color:' + esc(e.projectColor || '#4A90D9') + '">' +
          esc(e.projectName) + '</span>' : '') +
        '</div>';
    }).join('');
    return '<div class="day-card ' + (cls || '') + '"><div class="dc-head">' + title +
      '<span class="cnt">' + (items || []).length + '</span></div>' +
      (rows || '<div class="empty-tip">' + esc(emptyTip) + '</div>') + '</div>';
  }

  side.innerHTML = sideCard('全天任务', data.allday, '', '当日无全天任务') +
    sideCard('已完成', data.done, 'done', '当日暂无完成记录');
  layout.appendChild(side);
  content.appendChild(layout);

  // 滚动到当前时刻附近（今天）
  if (isToday) {
    const n = new Date();
    timeline.scrollTop =
      Math.max(0, (n.getHours() * 60 + n.getMinutes()) / 60 * DAY_HOUR_PX - 160);
  }
}

/* ---------------- 专注模式（全屏番茄钟） ---------------- */
let focusTimer = null;
let focusKeyHandler = null;

async function enterFocusMode(taskId) {
  if ($('#focus-root')) return;
  if (taskId) POMO.taskId = taskId;
  if (!POMO.taskId) {
    // 未指定任务：取今日第一个未完成任务
    try {
      const r = await apiGet('/api/today');
      const pool = (r.overdue || []).concat(r.dueToday || [], r.startToday || [], r.noDate || []);
      if (pool.length) POMO.taskId = pool[0].id;
    } catch (e) { /* 忽略 */ }
  }
  let task = null;
  if (POMO.taskId) {
    try { task = await apiGet('/api/tasks/' + POMO.taskId); } catch (e) { /* 忽略 */ }
  }

  const ov = document.createElement('div');
  ov.className = 'focus-overlay';
  ov.id = 'focus-root';
  ov.innerHTML =
    '<div class="focus-box">' +
    '<div class="focus-task" id="focus-task">' +
      (task ? esc(task.title) : '自由专注（未关联任务）') + '</div>' +
    '<div class="focus-timer" id="focus-timer">25:00</div>' +
    '<div class="focus-btns">' +
      '<button class="btn btn-primary btn-lg" id="focus-start">开始专注</button>' +
      '<button class="btn btn-lg" id="focus-reset">重置</button>' +
      (task && task.status !== 'done'
        ? '<button class="btn btn-lg" id="focus-done">✓ 完成任务</button>' : '') +
    '</div>' +
    '<div class="focus-tip">Esc 退出专注模式 · 番茄钟与右下角挂件联动</div>' +
    '</div>';
  document.body.appendChild(ov);

  function updateFocusUI() {
    const m = Math.floor(POMO.remain / 60), s = POMO.remain % 60;
    const t = $('#focus-timer');
    if (t) t.textContent = String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
    const b = $('#focus-start');
    if (b) b.textContent = POMO.running ? '暂停' : (POMO.remain < POMO.total ? '继续' : '开始专注');
  }
  updateFocusUI();
  clearInterval(focusTimer);
  focusTimer = setInterval(updateFocusUI, 500);

  $('#focus-start').addEventListener('click', pomoToggle);
  $('#focus-reset').addEventListener('click', () => {
    POMO.running = false;
    if (POMO.timer) { clearInterval(POMO.timer); POMO.timer = null; }
    POMO.remain = POMO.total;
    renderPomo();
    updateFocusUI();
  });
  const btnDone = $('#focus-done');
  if (btnDone) btnDone.addEventListener('click', async () => {
    try {
      const r = await api('POST', '/api/tasks/' + POMO.taskId + '/complete');
      toast(r.nextInstance && r.nextInstance.id ? '已完成，已生成下一次' : '已完成', 'ok');
      exitFocusMode();
      await refreshAll();
    } catch (e) { toast(e.message, 'err'); }
  });

  focusKeyHandler = e => {
    if (e.key === 'Escape') { e.preventDefault(); exitFocusMode(); }
  };
  document.addEventListener('keydown', focusKeyHandler);
}

function exitFocusMode() {
  var ov = $('#focus-root');
  if (ov && !ov.classList.contains('closing')) {
    ov.classList.add('closing');
    setTimeout(function() { if (ov && ov.parentNode) ov.remove(); }, 220);
  } else if (ov) {
    ov.remove();
  }
  clearInterval(focusTimer);
  focusTimer = null;
  if (focusKeyHandler) {
    document.removeEventListener('keydown', focusKeyHandler);
    focusKeyHandler = null;
  }
}

/* ---------------- 多端同步（备份快照合并） ---------------- */
function openSyncModal() {
  const body = document.createElement('div');
  body.className = 'sync-box';
  body.innerHTML =
    '<div class="sync-step"><div class="ss-title">1. 选择备份快照文件</div>' +
    '<div class="ss-desc">在另一台设备上「导出 → 完整备份快照」得到 .json 文件，在此选择该文件。' +
    '合并策略：任务按 id 对齐、更新时间新者胜；项目/标签按名称合并；模板/筛选/节假日去重追加；' +
    '本地已有数据不会被删除。</div>' +
    '<input type="file" id="sync-file" accept=".json,application/json" class="sync-file"></div>' +
    '<div class="sync-step"><div class="ss-title">2. 快照预览</div>' +
    '<div id="sync-preview" class="sync-preview empty-tip">尚未选择文件</div></div>' +
    '<div class="sync-step"><div class="ss-title">3. 合并结果</div>' +
    '<div id="sync-result"></div></div>';

  const foot = document.createElement('div');
  foot.style.cssText = 'display:flex;gap:8px;width:100%';
  const btnCancel = document.createElement('button');
  btnCancel.className = 'btn';
  btnCancel.textContent = '取消';
  btnCancel.addEventListener('click', closeModal);
  const btnGo = document.createElement('button');
  btnGo.className = 'btn btn-primary';
  btnGo.textContent = '开始合并';
  btnGo.disabled = true;
  foot.appendChild(btnCancel);
  foot.appendChild(btnGo);

  openModal('多端同步（合并备份快照）', body, foot);

  let snap = null;
  $('#sync-file').addEventListener('change', e => {
    const f = e.target.files && e.target.files[0];
    const prev = $('#sync-preview');
    snap = null;
    btnGo.disabled = true;
    if (!f) { prev.textContent = '尚未选择文件'; return; }
    const reader = new FileReader();
    reader.onload = () => {
      try {
        const d = JSON.parse(reader.result);
        if (d.app !== 'cpp-todo' || !Array.isArray(d.tasks)) {
          prev.textContent = '无效文件：应为「完整备份快照」导出的 .json';
          prev.className = 'sync-preview sync-err';
          return;
        }
        snap = d;
        prev.className = 'sync-preview';
        prev.innerHTML = '导出时间：' + esc(d.exportedAt || '未知') + '<br>' +
          '任务 ' + d.tasks.length + ' 条 · 项目 ' + (d.projects || []).length +
          ' 个 · 标签 ' + (d.tags || []).length + ' 个 · 模板 ' + (d.templates || []).length + ' 个';
        btnGo.disabled = false;
      } catch (err) {
        prev.textContent = 'JSON 解析失败：' + err.message;
        prev.className = 'sync-preview sync-err';
      }
    };
    reader.readAsText(f);
  });

  btnGo.addEventListener('click', async () => {
    if (!snap) return;
    var restore = btnLoading(btnGo);
    try {
      const r = await api('POST', '/api/sync', snap);
      $('#sync-result').innerHTML = '<div class="sync-ok">\u2713 ' + esc(r.summary || '合并完成') + '</div>';
      toast('同步完成', 'ok');
      await refreshAll();
      setTimeout(closeModal, 1800);
    } catch (err) {
      $('#sync-result').innerHTML = '<div class="sync-err">\u2715 ' + esc(err.message) + '</div>';
    }
    finally { restore(); }
  });
}

/* ---------------- 拖拽排序 ---------------- */
function bindDragEvents() {
  const content = $('#content');
  content.addEventListener('dragstart', e => {
    const row = e.target.closest('.task-row');
    if (!row || S.batchMode) return;
    S.dragTaskId = row.dataset.id;
    row.classList.add('dragging');
    try { e.dataTransfer.effectAllowed = 'move'; } catch (x) { /* 忽略 */ }
  });
  content.addEventListener('dragend', e => {
    const row = e.target.closest('.task-row');
    if (row) row.classList.remove('dragging');
    $$('.task-row.drag-over', content).forEach(r => r.classList.remove('drag-over'));
    S.dragTaskId = null;
  });
  content.addEventListener('dragover', e => {
    if (!S.dragTaskId) return;
    const row = e.target.closest('.task-row');
    if (!row || row.dataset.id === S.dragTaskId) return;
    e.preventDefault();
    $$('.task-row.drag-over', content).forEach(r => r.classList.remove('drag-over'));
    row.classList.add('drag-over');
  });
  content.addEventListener('drop', async e => {
    if (!S.dragTaskId) return;
    e.preventDefault();
    const row = e.target.closest('.task-row');
    if (!row || row.dataset.id === S.dragTaskId) return;
    const list = row.parentElement;
    if (!list) return;
    const dragged = list.querySelector('.task-row[data-id="' + S.dragTaskId + '"]');
    if (!dragged) return;
    // 判断插入到目标前/后
    const rows = Array.from(list.querySelectorAll('.task-row'));
    const fromIdx = rows.indexOf(dragged);
    const toIdx = rows.indexOf(row);
    if (toIdx > fromIdx) row.after(dragged);
    else row.before(dragged);
    const ids = Array.from(list.querySelectorAll('.task-row')).map(r => parseInt(r.dataset.id, 10));
    try {
      await api('POST', '/api/tasks/reorder', { ids: ids });
      toast('顺序已保存');
    } catch (err) {
      toast(err.message, 'err');
      await refreshAll();
    }
  });
}

/* ---------------- 快捷键 ---------------- */
const KEY_VIEWS = ['today', 'projects', 'tags', 'calendar', 'kanban', 'filters', 'stats', 'gantt', 'trash'];

function bindShortcuts() {
  document.addEventListener('keydown', e => {
    const tag = (e.target.tagName || '').toLowerCase();
    const inField = tag === 'input' || tag === 'textarea' || tag === 'select' || e.target.isContentEditable;
    // 命令面板快捷键优先
    if ((e.metaKey || e.ctrlKey) && (e.key === 'k' || e.key === 'K')) {
      e.preventDefault();
      openPalette();
      return;
    }
    // 撤销（输入框内不拦截，保留文本编辑的原生撤销）
    if ((e.metaKey || e.ctrlKey) && !e.shiftKey && !e.altKey && (e.key === 'z' || e.key === 'Z')) {
      e.preventDefault();
      undoLast();
      return;
    }
    if (inField) return;
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    if (e.key === 'n' || e.key === 'N') { e.preventDefault(); openTaskEditor(null); }
    else if (e.key === '/') { e.preventDefault(); $('#search-input').focus(); }
    else if (e.key === 'p' || e.key === 'P') {
      const w = $('#pomo-widget');
      if (w.classList.contains('hidden')) showPomodoro();
      else w.classList.add('hidden');
    }
    else if (e.key === 'd' || e.key === 'D') { e.preventDefault(); switchView('day'); }
    else if (e.key === 'f' || e.key === 'F') { e.preventDefault(); enterFocusMode(); }
    else if (e.key === 't' || e.key === 'T') { toggleTheme(); }
    else if (e.key === 'q' || e.key === 'Q') { e.preventDefault(); const q = $('#quick-input'); if (q) q.focus(); }
    else if (e.key === '?') { openHelpModal(); }
    else if (e.key >= '1' && e.key <= '9') {
      const v = KEY_VIEWS[parseInt(e.key, 10) - 1];
      if (v) switchView(v);
    }
  });
}


let modalKeyHandler = null;

function openModal(title, bodyEl, footEl, wide, noTitle) {
  const root = $('#modal-root');
  root.innerHTML = '';
  const mask = document.createElement('div');
  mask.className = 'modal-mask';
  const modal = document.createElement('div');
  modal.className = 'modal' + (wide ? ' wide' : '');
  modal.innerHTML = '<div class="modal-head">' +
    (noTitle ? '' : '<h3>' + esc(title) + '</h3>') +
    '<button class="modal-close" data-act="close-modal">✕</button></div>';
  const body = document.createElement('div');
  body.className = 'modal-body';
  if (bodyEl) body.appendChild(bodyEl);
  modal.appendChild(body);
  if (footEl) {
    const foot = document.createElement('div');
    foot.className = 'modal-foot';
    foot.appendChild(footEl);
    modal.appendChild(foot);
  }
  mask.appendChild(modal);
  root.appendChild(mask);
  mask.addEventListener('click', e => { if (e.target === mask) closeModal(); });
  modalKeyHandler = e => { if (e.key === 'Escape') closeModal(); };
  document.addEventListener('keydown', modalKeyHandler);
  return modal;
}

function closeModal() {
  var root = $('#modal-root');
  var mask = root.querySelector('.modal-mask');
  if (mask && !mask.classList.contains('closing')) {
    mask.classList.add('closing');
    setTimeout(function() { root.innerHTML = ''; }, 180);
  } else {
    root.innerHTML = '';
  }
  if (modalKeyHandler) {
    document.removeEventListener('keydown', modalKeyHandler);
    modalKeyHandler = null;
  }
}

/* ---------------- 任务编辑器 ---------------- */
/* task：编辑时传任务对象；新建但需预填字段（如日视图时段点击）时传 {..} 且 forceNew=true */
async function openTaskEditor(task, forceNew) {
  const editing = forceNew ? false : !!task;
  const t = task || {};
  const tagsAll = S.tags;

  // 全部任务（供父任务/依赖选择）
  S.taskIndex.clear();
  const treeData = await apiGet('/api/tree');
  buildTaskIndex(treeData.tree || [], S.taskIndex);

  const body = document.createElement('div');
  const form = document.createElement('div');
  form.innerHTML =
    '<div class="tf-grid">' +
    '<div class="tf tf-full"><label>标题 *</label><input id="tf-title" value="' + esc(t.title || '') + '" placeholder="要做什么？"></div>' +

    '<div class="tf tf-full"><label>备注（Markdown）</label>' +
    '<div class="tabs"><button type="button" class="tab-edit active" data-act="tab-edit">编辑</button>' +
    '<button type="button" class="tab-prev" data-act="tab-prev">预览</button></div>' +
    '<textarea id="tf-notes" placeholder="支持 Markdown：**加粗**、- 列表、`代码`…">' + esc(t.notes || '') + '</textarea>' +
    '<div class="md-preview hidden" id="tf-preview"></div></div>' +

    '<div class="tf"><label>优先级</label><select id="tf-prio">' +
    '<option value="2"' + (t.priority === 2 ? ' selected' : '') + '>高</option>' +
    '<option value="1"' + (t.priority === 1 || t.priority === undefined ? ' selected' : '') + '>中</option>' +
    '<option value="0"' + (t.priority === 0 ? ' selected' : '') + '>低</option>' +
    '</select></div>' +
    '<div class="tf"><label>状态</label><select id="tf-status">' +
    '<option value="todo"' + ((t.status || 'todo') === 'todo' ? ' selected' : '') + '>待办</option>' +
    '<option value="doing"' + (t.status === 'doing' ? ' selected' : '') + '>进行中</option>' +
    '<option value="done"' + (t.status === 'done' ? ' selected' : '') + '>已完成</option>' +
    '<option value="archived"' + (t.status === 'archived' ? ' selected' : '') + '>已归档</option>' +
    '</select></div>' +

    '<div class="tf"><label>截止日期</label><input type="date" id="tf-due" value="' + esc(t.dueDate || '') + '"></div>' +
    '<div class="tf"><label>开始日期</label><input type="date" id="tf-start" value="' + esc(t.startDate || '') + '"></div>' +
    '<div class="tf"><label>时间提醒</label><input type="time" id="tf-time" value="' + esc(t.remindTime || '') + '"></div>' +
    '<div class="tf"><label>项目</label><select id="tf-project"><option value="0">（无）</option>' +
    S.projects.map(p => '<option value="' + p.id + '"' + (t.projectId === p.id ? ' selected' : '') + '>' +
      esc(p.name) + '</option>').join('') + '</select></div>' +

    '<div class="tf"><label>父任务（设为子任务）</label><select id="tf-parent"><option value="0">（无）</option>' +
    Array.from(S.taskIndex.values()).filter(x => x.id !== t.id).map(x =>
      '<option value="' + x.id + '"' + (t.parentId === x.id ? ' selected' : '') + '>' +
      '#' + x.id + ' ' + esc(x.title.slice(0, 24)) + '</option>').join('') + '</select></div>' +

    '<div class="tf"><label>农历提醒（M-D）</label>' +
    '<div style="display:flex;gap:8px;align-items:center">' +
    '<input type="checkbox" id="tf-lunar-ck"' + (t.lunarRemind ? ' checked' : '') + ' style="width:16px;height:16px;accent-color:var(--primary)">' +
    '<input type="text" id="tf-lunar" placeholder="如 5-5 端午 / 8-15 中秋" value="' + esc(t.lunarDate || '') + '" style="flex:1">' +
    '</div><span class="hint">农历 M-D 格式；勾选后按农历每年提醒</span></div>' +

    '<div class="tf"><label>重复规则</label><select id="tf-freq">' +
    '<option value="">不重复</option>' +
    '<option value="daily"' + (t.repeatRule && t.repeatRule.freq === 'daily' ? ' selected' : '') + '>每天</option>' +
    '<option value="weekly"' + (t.repeatRule && t.repeatRule.freq === 'weekly' ? ' selected' : '') + '>每周</option>' +
    '<option value="monthly"' + (t.repeatRule && t.repeatRule.freq === 'monthly' ? ' selected' : '') + '>每月</option>' +
    '<option value="yearly"' + (t.repeatRule && t.repeatRule.freq === 'yearly' ? ' selected' : '') + '>每年</option>' +
    '<option value="custom"' + (t.repeatRule && t.repeatRule.freq === 'custom' ? ' selected' : '') + '>自定义周期</option>' +
    '</select></div>' +
    '</div>' +

    '<div id="tf-repeat-box" class="hidden"><div class="sec-title">重复参数</div><div class="tf-grid">' +
    '<div class="tf"><label>每 N 个周期</label><input type="number" id="tf-interval" min="1" value="' +
    ((t.repeatRule && t.repeatRule.interval) || 1) + '"></div>' +
    '<div class="tf"><label>结束日期（可选）</label><input type="date" id="tf-enddate" value="' +
    esc((t.repeatRule && t.repeatRule.endDate) || '') + '"></div>' +
    '<div class="tf tf-full" id="tf-wd-row" style="display:none"><label>每周几</label>' +
    '<div class="wd-row">' +
    ['一', '二', '三', '四', '五', '六', '日'].map((w, i) =>
      '<button type="button" class="wd-box" data-wd="' + (i + 1) + '">' + w + '</button>').join('') +
    '</div></div>' +
    '<div class="tf tf-check"><input type="checkbox" id="tf-sk-wk"' +
    (t.repeatRule && t.repeatRule.skipWeekends ? ' checked' : '') + '><label>跳过周末</label></div>' +
    '<div class="tf tf-check"><input type="checkbox" id="tf-sk-hd"' +
    (t.repeatRule && t.repeatRule.skipHolidays ? ' checked' : '') + '><label>跳过节假日</label></div>' +
    '</div></div>' +

    '<div class="sec-title">标签</div>' +
    '<div id="tf-tags" style="display:flex;flex-wrap:wrap;gap:6px">' +
    tagsAll.map(tg => {
      const on = (t.tags || []).some(x => x.id === tg.id);
      return '<label class="tag-pill" style="cursor:pointer;display:inline-flex;align-items:center;gap:4px;' +
        (on ? 'background:var(--primary-weak);border-color:var(--primary)' : '') + '">' +
        '<input type="checkbox" class="tg-ck" value="' + esc(tg.name) + '"' + (on ? ' checked' : '') +
        ' style="accent-color:var(--primary)">' + esc(tg.name) + '</label>';
    }).join('') + '</div>' +
    '<div style="display:flex;gap:8px;margin-top:8px">' +
    '<input id="tf-newtag" placeholder="新标签名，回车添加" style="flex:1;padding:7px 10px;border:1px solid var(--border);border-radius:7px">' +
    '<button type="button" class="btn btn-sm" data-act="add-tag">添加</button></div>';

  body.appendChild(form);

  // 依赖区块
  const depSec = document.createElement('div');
  depSec.innerHTML = '<div class="sec-title">任务依赖（依赖完成才能开始本任务）</div>';
  const depBox = document.createElement('div');
  depBox.className = 'dep-list';
  depSec.appendChild(depBox);
  body.appendChild(depSec);

  function renderDeps() {
    const deps = t.dependsOn || [];
    depBox.innerHTML = deps.length ? deps.map(d =>
      '<div class="dep-item"><span>' + esc(d.title) + '</span>' +
      '<span class="dep-status ' + (d.status === 'done' ? 'st-done' : 'st-open') + '">' +
      (d.status === 'done' ? '已完成' : '未完成') + '</span>' +
      '<button class="dep-x" data-act="del-dep" data-id="' + d.id + '">✕</button></div>').join('')
      : '<div class="empty-tip" style="padding:6px 4px">无依赖。</div>';
    if (editing) {
      const add = document.createElement('div');
      add.className = 'dep-add';
      add.innerHTML = '<select id="dep-select"><option value="0">选择要依赖的任务…</option>' +
        Array.from(S.taskIndex.values()).filter(x => x.id !== t.id && !deps.some(d => d.id === x.id))
          .map(x => '<option value="' + x.id + '">#' + x.id + ' ' + esc(x.title.slice(0, 30)) + '</option>').join('') +
        '</select><button class="btn btn-sm" data-act="add-dep">添加</button>';
      depBox.appendChild(add);
    } else {
      const tip = document.createElement('div');
      tip.className = 'empty-tip';
      tip.textContent = '（保存后可在编辑中设置依赖）';
      depBox.appendChild(tip);
    }
  }
  renderDeps();

  // 子任务区块
  if (editing && t.children && t.children.length) {
    const subSec = document.createElement('div');
    subSec.innerHTML = '<div class="sec-title">子任务</div>' +
      '<div class="sub-list">' + t.children.map(c =>
        '<div class="sub-item"><span class="' + (c.status === 'done' ? 'si-done' : '') + '">' +
        esc(c.title) + '</span><span class="dep-status ' + (c.status === 'done' ? 'st-done' : 'st-open') + '">' +
        STATUS_LABEL[c.status] + '</span></div>').join('') + '</div>';
    body.appendChild(subSec);
  }

  // 备注 tab 切换
  const notesTa = $('#tf-notes', form);
  const preview = $('#tf-preview', form);
  form.addEventListener('click', e => {
    const act = e.target.closest('[data-act]');
    if (!act) return;
    if (act.dataset.act === 'tab-edit') {
      act.classList.add('active');
      act.nextElementSibling.classList.remove('active');
      notesTa.classList.remove('hidden');
      preview.classList.add('hidden');
    } else if (act.dataset.act === 'tab-prev') {
      act.classList.add('active');
      act.previousElementSibling.classList.remove('active');
      notesTa.classList.add('hidden');
      preview.classList.remove('hidden');
      preview.innerHTML = md(notesTa.value);
    }
  });

  // 重复规则交互
  const freqSel = $('#tf-freq', form);
  const repBox = $('#tf-repeat-box', form);
  const wdRow = $('#tf-wd-row', form);
  const wdSet = new Set((t.repeatRule && t.repeatRule.weekdays) || []);
  function syncRepeatUI() {
    const f = freqSel.value;
    repBox.classList.toggle('hidden', !f);
    wdRow.style.display = (f === 'weekly' || f === 'custom') ? '' : 'none';
  }
  freqSel.addEventListener('change', syncRepeatUI);
  syncRepeatUI();
  wdRow.querySelectorAll('.wd-box').forEach(b => {
    const w = parseInt(b.dataset.wd, 10);
    if (wdSet.has(w)) b.classList.add('on');
    b.addEventListener('click', () => {
      b.classList.toggle('on');
    });
  });

  // 重复规则预览：显示规则接下来命中的日期
  const repPrev = document.createElement('div');
  repPrev.className = 'tf-rep-preview';
  repPrev.id = 'tf-rep-preview';
  repBox.appendChild(repPrev);
  let repPrevTimer = null;
  function collectRepeatRuleForPreview() {
    const f = freqSel.value;
    if (!f) return null;
    const r = { freq: f, interval: Math.max(1, parseInt($('#tf-interval').value, 10) || 1) };
    if (f === 'weekly' || f === 'custom') {
      const wd = Array.from(wdRow.querySelectorAll('.wd-box.on'))
        .map(b => parseInt(b.dataset.wd, 10)).sort((a, b) => a - b);
      if (wd.length) r.weekdays = wd;
    }
    r.skipWeekends = $('#tf-sk-wk').checked;
    r.skipHolidays = $('#tf-sk-hd').checked;
    r.endDate = $('#tf-enddate').value;
    return r;
  }
  async function updateRepPreview() {
    const rule = collectRepeatRuleForPreview();
    if (!rule) { repPrev.innerHTML = ''; return; }
    if ((rule.freq === 'weekly' || rule.freq === 'custom') && !(rule.weekdays && rule.weekdays.length)) {
      repPrev.innerHTML = '<span class="rp-hint">请先勾选每周几，预览将显示命中日期</span>';
      return;
    }
    try {
      const from = $('#tf-due').value || S.today;
      const r = await api('POST', '/api/repeat-preview', { rule: rule, from: from, count: 6 });
      const dates = r.dates || [];
      repPrev.innerHTML = dates.length
        ? '<span class="rp-label">' + svgIcon('calendar', 12) + ' 接下来</span>' +
          dates.map(d => '<span class="rp-date">' + fmtDate(d) + '</span>').join('<span class="rp-sep">·</span>')
        : '<span class="rp-hint">该规则在此起点下没有未来实例</span>';
    } catch (e) {
      repPrev.innerHTML = '<span class="rp-hint">预览失败：' + esc(e.message) + '</span>';
    }
  }
  function queueRepPreview() {
    clearTimeout(repPrevTimer);
    repPrevTimer = setTimeout(updateRepPreview, 250);
  }
  freqSel.addEventListener('change', queueRepPreview);
  ['#tf-interval', '#tf-enddate', '#tf-due'].forEach(id => {
    const el = $(id, form);
    if (el) el.addEventListener('input', queueRepPreview);
  });
  ['#tf-sk-wk', '#tf-sk-hd'].forEach(id => {
    const el = $(id, form);
    if (el) el.addEventListener('change', queueRepPreview);
  });
  wdRow.querySelectorAll('.wd-box').forEach(b => b.addEventListener('click', queueRepPreview));
  updateRepPreview();

  // 新标签
  const newTagInput = $('#tf-newtag', form);
  newTagInput.addEventListener('keydown', e => {
    if (e.key === 'Enter') { e.preventDefault(); addNewTag(); }
  });
  form.addEventListener('click', e => {
    const act = e.target.closest('[data-act]');
    if (!act) return;
    if (act.dataset.act === 'add-tag') addNewTag();
  });
  function addNewTag() {
    const name = newTagInput.value.trim();
    if (!name) return;
    const ck = document.createElement('label');
    ck.className = 'tag-pill';
    ck.style.cssText = 'cursor:pointer;display:inline-flex;align-items:center;gap:4px;background:var(--primary-weak);border-color:var(--primary)';
    ck.innerHTML = '<input type="checkbox" class="tg-ck" value="' + esc(name) + '" checked style="accent-color:var(--primary)">' + esc(name);
    $('#tf-tags', form).appendChild(ck);
    newTagInput.value = '';
  }

  // 依赖操作
  body.addEventListener('click', async e => {
    const act = e.target.closest('[data-act]');
    if (!act) return;
    if (act.dataset.act === 'add-dep') {
      const sel = $('#dep-select');
      const depId = parseInt(sel.value, 10);
      if (!depId) { toast('请选择依赖任务', 'err'); return; }
      try {
        const r = await api('POST', '/api/tasks/' + t.id + '/deps', { dependsOn: depId });
        toast('依赖已添加');
        t.dependsOn = r.task.dependsOn || t.dependsOn;
        renderDeps();
      } catch (err) { toast(err.message, 'err'); }
    } else if (act.dataset.act === 'del-dep') {
      const depId = parseInt(act.dataset.id, 10);
      try {
        await api('DELETE', '/api/tasks/' + t.id + '/deps/' + depId);
        toast('依赖已移除');
        t.dependsOn = (t.dependsOn || []).filter(d => d.id !== depId);
        renderDeps();
      } catch (err) { toast(err.message, 'err'); }
    }
  });

  // 底部按钮
  const foot = document.createElement('div');
  foot.style.cssText = 'display:flex;gap:8px;justify-content:flex-end;width:100%';
  const btnCancel = document.createElement('button');
  btnCancel.className = 'btn';
  btnCancel.textContent = '取消';
  btnCancel.addEventListener('click', closeModal);
  const btnTpl = document.createElement('button');
  btnTpl.className = 'btn';
  btnTpl.textContent = '存为模板';
  btnTpl.title = '把当前填写的内容保存为任务模板';
  btnTpl.addEventListener('click', () => {
    const payload = collectTask(t);
    if (!payload.title) { toast('请先填写标题', 'err'); return; }
    openSaveTemplateModal(payload);
  });
  const btnSave = document.createElement('button');
  btnSave.className = 'btn btn-primary';
  btnSave.textContent = editing ? '保存修改' : '创建任务';
  foot.appendChild(btnCancel);
  foot.appendChild(btnTpl);
  foot.appendChild(btnSave);

  openModal(editing ? ('编辑任务 #' + t.id) : '新建任务', body, foot);

  btnSave.addEventListener('click', async () => {
    const payload = collectTask(t);
    if (!payload.title) { toast('标题不能为空', 'err'); return; }
    var restore = btnLoading(btnSave);
    try {
      if (editing) {
        await api('PUT', '/api/tasks/' + t.id, payload);
        toast('已保存', 'ok');
      } else {
        const r = await api('POST', '/api/tasks', payload);
        S.lastNewTaskId = r.task.id;
        toast('已创建 #' + r.task.id, 'ok');
      }
      closeModal();
      await refreshAll();
      if (S.lastNewTaskId) {
        var newRow = document.querySelector('.task-row[data-id="' + S.lastNewTaskId + '"]');
        if (newRow) newRow.classList.add('is-new');
        S.lastNewTaskId = null;
      }
    } catch (err) { toast('保存失败：' + err.message, 'err'); }
    finally { restore(); }
  });
}

function collectTask(t) {
  const g = id => $(id);
  const v = id => g(id).value;
  const ck = id => g(id).checked;
  const weekdays = Array.from(document.querySelectorAll('#tf-wd-row .wd-box.on'))
    .map(b => parseInt(b.dataset.wd, 10)).sort((a, b) => a - b);
  const tags = Array.from(document.querySelectorAll('.tg-ck:checked')).map(x => x.value);

  const repeatRule = {};
  const freq = v('#tf-freq');
  if (freq) {
    repeatRule.freq = freq;
    repeatRule.interval = Math.max(1, parseInt(v('#tf-interval'), 10) || 1);
    if (freq === 'weekly' || freq === 'custom') {
      repeatRule.weekdays = weekdays.length ? weekdays : [new Date(S.today).getDay() === 0 ? 7 : new Date(S.today).getDay()];
    }
    repeatRule.skipWeekends = ck('#tf-sk-wk');
    repeatRule.skipHolidays = ck('#tf-sk-hd');
    repeatRule.endDate = v('#tf-enddate');
    repeatRule.maxInstances = 0;
  }

  const lunarDate = v('#tf-lunar').trim();
  const hasLunar = ck('#tf-lunar-ck') && lunarDate !== '';
  const remindTime = v('#tf-time');

  const payload = {
    title: v('#tf-title').trim(),
    notes: v('#tf-notes'),
    priority: parseInt(v('#tf-prio'), 10),
    status: v('#tf-status'),
    startDate: v('#tf-start'),
    dueDate: v('#tf-due'),
    remindTime: remindTime,
    hasReminder: ck('#tf-lunar-ck') ? (!!remindTime || hasLunar) : !!remindTime,
    lunarRemind: hasLunar,
    lunarDate: hasLunar ? lunarDate : '',
    projectId: parseInt(v('#tf-project'), 10),
    parentId: parseInt(v('#tf-parent'), 10),
    repeatRule: repeatRule,
    tags: tags
  };
  return payload;
}

/* ---------------- 导入弹窗 ---------------- */
function openImportModal() {
  const body = document.createElement('div');
  body.className = 'imp-grid';
  body.innerHTML =
    '<div class="tf"><label>数据来源格式</label><select id="imp-format">' +
    '<option value="todotxt">Todo.txt（每行一个任务）</option>' +
    '<option value="todoist">Todoist JSON 导出</option>' +
    '<option value="ticktick">滴答清单 JSON 导出</option>' +
    '<option value="csv">滴答清单 CSV 导出</option>' +
    '</select></div>' +
    '<div class="tf"><label>粘贴内容</label>' +
    '<textarea id="imp-text" placeholder="将导出的文本粘贴到这里…"></textarea></div>' +
    '<div class="tf"><label>格式示例</label>' +
    '<pre style="background:var(--panel-2);border:1px solid var(--border);border-radius:7px;padding:10px;font-size:12px;font-family:var(--mono);line-height:1.6">' +
    esc('x 2026-01-01 2026-01-15 完成调研 +项目A @工作 due:2026-01-15\n买菜 +生活 @日常\n\n# Todoist / 滴答：整段粘贴其 JSON/CSV 导出内容即可') +
    '</pre></div>';

  const foot = document.createElement('div');
  foot.style.cssText = 'display:flex;gap:8px;width:100%';
  const btnCancel = document.createElement('button');
  btnCancel.className = 'btn';
  btnCancel.textContent = '取消';
  btnCancel.addEventListener('click', closeModal);
  const btnGo = document.createElement('button');
  btnGo.className = 'btn btn-primary';
  btnGo.textContent = '开始导入';
  foot.appendChild(btnCancel);
  foot.appendChild(btnGo);

  openModal('导入数据（滴答 / Todoist / Todo.txt）', body, foot);

  // 同步合并入口（备份快照）
  const syncLink = document.createElement('div');
  syncLink.className = 'sync-entry-link';
  syncLink.innerHTML = '<button type="button" class="btn btn-sm" data-act="sync-open">' +
    svgIcon('sync', 12) + ' 从另一台设备合并备份快照？点此多端同步</button>';
  body.appendChild(syncLink);

  btnGo.addEventListener('click', async () => {
    const format = $('#imp-format').value;
    const text = $('#imp-text').value.trim();
    if (!text) { toast('请粘贴内容', 'err'); return; }
    var restore = btnLoading(btnGo);
    try {
      const r = await fetch('/api/import?format=' + encodeURIComponent(format), {
        method: 'POST',
        headers: { 'Content-Type': 'text/plain' },
        body: text
      });
      const data = await r.json();
      const box = document.createElement('div');
      box.className = 'imp-result ' + (data.ok ? 'ok' : 'err');
      box.innerHTML = '<div class="ir-line">' + esc(data.summary || '') + '</div>' +
        (data.errors && data.errors.length
          ? data.errors.map(e => '<div class="ir-line">! ' + esc(e) + '</div>').join('')
          : '');
      body.appendChild(box);
      if (data.ok) {
        toast('导入完成', 'ok');
        setTimeout(() => { closeModal(); refreshAll(); }, 1500);
      }
    } catch (e) {
      toast('导入失败：' + e.message, 'err');
    }
    finally { restore(); }
  });
}

/* ---------------- 存储位置弹窗（U盘 / 可移动盘） ---------------- */
function fmtBytes(n) {
  if (!n || n <= 0) return '—';
  if (n >= 1073741824) return (n / 1073741824).toFixed(1) + ' GB';
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

async function openStorageModal() {
  const body = document.createElement('div');
  body.innerHTML = '<div class="loading">读取存储信息…</div>';

  const foot = document.createElement('div');
  foot.style.cssText = 'display:flex;gap:8px;width:100%';
  const btnCancel = document.createElement('button');
  btnCancel.className = 'btn';
  btnCancel.textContent = '关闭';
  btnCancel.addEventListener('click', closeModal);
  const btnGo = document.createElement('button');
  btnGo.className = 'btn btn-primary';
  btnGo.textContent = '迁移数据到此位置';
  btnGo.disabled = true;
  foot.appendChild(btnCancel);
  foot.appendChild(btnGo);

  openModal('存储位置', body, foot);

  let st = null, vols = null;
  try {
    [st, vols] = await Promise.all([apiGet('/api/storage'), apiGet('/api/storage/volumes')]);
  } catch (e) {
    body.innerHTML = '<div class="empty-tip">读取失败：' + esc(e.message) + '</div>';
    return;
  }

  body.innerHTML =
    '<div class="st-cur">' +
      '<label>当前数据库</label>' +
      '<div class="st-path">' + esc(st.dbPath) + '</div>' +
      '<div class="st-meta">' + fmtBytes(st.sizeBytes) +
        (st.usingConfig ? ' · 已设为默认位置' : ' · 未设置默认（下次启动用 ./data/todo.db）') +
      '</div>' +
    '</div>' +
    '<div class="st-vols"><label>可用卷（◈ 疑似U盘 / 可移动盘）</label>' +
    (vols.volumes.length
      ? vols.volumes.map(v =>
        '<label class="vol-item' + (v.removable ? ' removable' : '') + (v.writable ? '' : ' ro') + '">' +
          '<input type="radio" name="st-vol" value="' + esc(v.path) + '"' +
            (v.writable ? '' : ' disabled') + '>' +
          '<div class="vol-info">' +
            '<div class="vol-name">' + (v.removable ? '◈ ' : '') + esc(v.name) +
              (v.writable ? '' : ' <em>（只读）</em>') + '</div>' +
            '<div class="vol-meta">剩余 ' + fmtBytes(v.freeBytes) + ' / 总 ' + fmtBytes(v.totalBytes) + '</div>' +
          '</div>' +
        '</label>').join('')
      : '<div class="empty-tip">未发现可用卷。插入U盘后点击「刷新」重试。</div>') +
    '</div>' +
    '<div class="st-vols" style="margin-top:10px"><label>或手动指定路径</label>' +
      '<input id="st-custom" class="st-input" type="text" placeholder="/Volumes/我的U盘/todo.db（目录或 .db 文件均可）">' +
    '</div>' +
    '<div class="backup-list"><label style="display:block;font-size:11px;color:var(--text-3);margin:14px 0 6px;font-weight:600">' +
      '备份（每日启动自动备份 · 保留最近 7 份）</label>' +
      '<div style="display:flex;gap:8px;margin-bottom:8px">' +
        '<button class="btn btn-sm" data-act="backup-now" id="btn-backup-now">💾 立即备份</button>' +
        '<span id="bk-hint" style="font-size:11px;color:var(--text-3);align-self:center"></span>' +
      '</div>' +
      '<div id="bk-list"><div class="empty-tip" style="padding:4px 0">加载中…</div></div>' +
    '</div>' +
    '<div class="webdav-section" style="margin-top:14px;border-top:1px solid var(--border);padding-top:12px">' +
      '<label style="display:flex;align-items:center;gap:8px;cursor:pointer;font-size:13px;font-weight:600;color:var(--text-heading)">' +
        '<input type="checkbox" id="wd-enabled"> WebDAV 同步' +
      '</label>' +
      '<div id="wd-form" style="display:none;margin-top:10px">' +
        '<input id="wd-url" class="st-input" type="text" placeholder="WebDAV URL，如 https://dav.example.com/">' +
        '<input id="wd-username" class="st-input" type="text" placeholder="用户名" style="margin-top:6px">' +
        '<input id="wd-password" class="st-input" type="password" placeholder="密码" style="margin-top:6px">' +
        '<input id="wd-remote-dir" class="st-input" type="text" placeholder="远端目录（可选）" style="margin-top:6px">' +
        '<div style="display:flex;gap:8px;margin-top:6px">' +
          '<select id="wd-policy" class="st-input" style="flex:1">' +
            '<option value="newer">冲突策略：较新者胜</option>' +
            '<option value="local">冲突策略：本地优先</option>' +
            '<option value="remote">冲突策略：远端优先</option>' +
            '<option value="both">冲突策略：保留双方</option>' +
            '<option value="error">冲突策略：报错跳过</option>' +
          '</select>' +
          '<label style="display:flex;align-items:center;gap:4px;font-size:12px;white-space:nowrap;color:var(--text-2)">' +
            '<input type="checkbox" id="wd-propagate" checked> 传播删除' +
          '</label>' +
        '</div>' +
        '<div style="display:flex;gap:8px;margin-top:10px">' +
          '<button class="btn btn-sm btn-primary" data-act="wd-save">保存配置</button>' +
          '<button class="btn btn-sm" data-act="wd-sync" id="btn-wd-sync">立即同步</button>' +
        '</div>' +
        '<pre id="wd-log" style="margin-top:8px;display:none;background:var(--bg-2);padding:8px;border-radius:6px;font-size:11px;max-height:120px;overflow:auto;color:var(--text);border:1px solid var(--border)"></pre>' +
      '</div>' +
    '</div>';

  // 备份列表
  const loadBackups = async () => {
    const box = $('#bk-list', body);
    try {
      const bks = await apiGet('/api/backups');
      box.innerHTML = (bks.backups || []).length
        ? bks.backups.map(b =>
            '<div class="bk-item"><span>' + esc(b.name) + '</span>' +
            '<span class="bk-size">' + fmtBytes(b.sizeBytes) + '</span></div>').join('')
        : '<div class="empty-tip" style="padding:4px 0">暂无备份</div>';
    } catch (e) {
      box.innerHTML = '<div class="empty-tip" style="padding:4px 0">读取失败</div>';
    }
  };
  loadBackups();

  // ---- WebDAV 配置 ----
  const loadWebdav = async () => {
    try {
      const cfg = await apiGet('/api/webdav-config');
      $('#wd-enabled', body).checked = cfg.webdav_enabled;
      $('#wd-url', body).value = cfg.webdav_url || '';
      $('#wd-username', body).value = cfg.webdav_username || '';
      $('#wd-password', body).value = cfg.webdav_password || '';
      $('#wd-remote-dir', body).value = cfg.webdav_remote_dir || '';
      $('#wd-policy', body).value = cfg.webdav_conflict_policy || 'newer';
      $('#wd-propagate', body).checked = cfg.webdav_propagate_delete !== false;
      $('#wd-form', body).style.display = cfg.webdav_enabled ? 'block' : 'none';
    } catch (e) { /* ignore */ }
  };
  loadWebdav();

  $('#wd-enabled', body).addEventListener('change', () => {
    $('#wd-form', body).style.display = $('#wd-enabled', body).checked ? 'block' : 'none';
  });

  body.addEventListener('click', async e => {
    const saveBtn = e.target.closest('[data-act="wd-save"]');
    if (saveBtn) {
      saveBtn.disabled = true;
      try {
        await api('PUT', '/api/webdav-config', {
          enabled: $('#wd-enabled', body).checked,
          url: $('#wd-url', body).value.trim(),
          username: $('#wd-username', body).value.trim(),
          password: $('#wd-password', body).value,
          remoteDir: $('#wd-remote-dir', body).value.trim(),
          conflictPolicy: $('#wd-policy', body).value,
          propagateDelete: $('#wd-propagate', body).checked,
          timeout: 30
        });
        toast('WebDAV 配置已保存', 'ok');
      } catch (err) {
        toast('保存失败：' + err.message, 'err');
      }
      saveBtn.disabled = false;
      return;
    }
    const syncBtn = e.target.closest('[data-act="wd-sync"]');
    if (syncBtn) {
      const logBox = $('#wd-log', body);
      logBox.style.display = 'block';
      logBox.textContent = '同步中…';
      syncBtn.disabled = true;
      try {
        const r = await api('POST', '/api/webdav-sync');
        logBox.textContent = r.output || '同步完成';
        toast('WebDAV 同步完成', 'ok');
      } catch (err) {
        logBox.textContent = '同步失败：' + err.message;
        toast('同步失败：' + err.message, 'err');
      }
      syncBtn.disabled = false;
      return;
    }
    const b = e.target.closest('[data-act="backup-now"]');
    if (!b) return;
    b.disabled = true;
    try {
      await api('POST', '/api/backups');
      $('#bk-hint', body).textContent = '备份完成 ✓';
      toast('备份完成（数据库目录 /backups）', 'ok');
      await loadBackups();
    } catch (err) {
      $('#bk-hint', body).textContent = '';
      toast('备份失败：' + err.message, 'err');
    }
    b.disabled = false;
  });

  const radioSel = () => {
    const r = body.querySelector('input[name="st-vol"]:checked');
    return r ? r.value : '';
  };
  const updateTarget = () => {
    const custom = $('#st-custom', body).value.trim();
    btnGo.disabled = !radioSel() && !custom;
  };
  body.addEventListener('change', updateTarget);
  body.addEventListener('input', updateTarget);
  // 手动输入时清除卷选择
  $('#st-custom', body).addEventListener('focus', () => {
    $$('input[name="st-vol"]', body).forEach(r => { r.checked = false; });
    updateTarget();
  });

  btnGo.addEventListener('click', async () => {
    const target = $('#st-custom', body).value.trim() || radioSel();
    if (!target) { toast('请先选择一个卷或输入路径', 'err'); return; }
    if (!confirm('将把全部待办数据迁移到：\n' + target +
                 '\n\n原数据库文件会保留为备份，确认迁移？')) return;
    btnGo.disabled = true;
    btnGo.textContent = '迁移中…';
    try {
      const r = await api('POST', '/api/storage/move', { path: target });
      toast('已迁移到 ' + r.newDbPath);
      btnGo.textContent = '迁移完成 ✓';
      setTimeout(() => { closeModal(); refreshAll(); }, 1200);
    } catch (e) {
      let msg = e.message;
      let doOverwrite = false;
      if (msg.indexOf('需确认覆盖') >= 0) {
        doOverwrite = confirm('目标位置已存在数据库文件：\n' + msg +
                               '\n\n覆盖它？（原文件会先被本次数据替换，当前数据不受影响）');
        if (!doOverwrite) { btnGo.disabled = false; btnGo.textContent = '迁移数据到此位置'; return; }
        try {
          const r = await api('POST', '/api/storage/move', { path: target, overwrite: true });
          toast('已覆盖迁移到 ' + r.newDbPath);
          btnGo.textContent = '迁移完成 ✓';
          setTimeout(() => { closeModal(); refreshAll(); }, 1200);
          return;
        } catch (e2) { msg = e2.message; }
      }
      toast('迁移失败：' + msg, 'err');
      btnGo.disabled = false;
      btnGo.textContent = '迁移数据到此位置';
    }
  });
}

/* ---------------- 刷新 ---------------- */
async function refreshAll() {
  await refreshSidebar();
  await switchView(S.view);
}

/* ---------------- 事件绑定 ---------------- */
function bindEvents() {
  // 入场动效结束清理：移除 both 填充残留，避免 transform 锁死干扰 hover / 拖拽
  $('#content').addEventListener('animationend', e => {
    if (e.animationName !== 'enter-up') return;
    const t = e.target;
    if (t.classList && (t.classList.contains('anim-enter') || t.classList.contains('anim-row')))
      t.classList.remove('anim-enter', 'anim-row');
  });

  // 视图导航（含番茄钟快捷入口）
  $('#view-nav').addEventListener('click', e => {
    const b = e.target.closest('.nav-item');
    if (!b) return;
    if (b.dataset.view) switchView(b.dataset.view);
    else if (b.dataset.act === 'open-pomo') showPomodoro();
  });

  // 新建 / 导入 / 存储 / 主题 / 导出 / 命令面板
  $('#btn-new').addEventListener('click', () => openTaskEditor(null));
  $('#btn-import').addEventListener('click', openImportModal);
  $('#btn-storage').addEventListener('click', openStorageModal);
  $('#btn-theme').addEventListener('click', toggleTheme);
  $('#btn-export').addEventListener('click', openExportModal);
  $('#btn-palette').addEventListener('click', openPalette);

  // 快速录入（自然语言）：Enter 提交
  const qi = $('#quick-input');
  if (qi) {
    qi.addEventListener('keydown', e => {
      if (e.key === 'Enter') {
        const v = qi.value.trim();
        if (v) { quickAdd(v); qi.value = ''; }
      } else if (e.key === 'Escape') { qi.value = ''; qi.blur(); }
    });
  }
  bindDragEvents();
  bindShortcuts();

  // 侧边栏：项目树 / 标签
  $('#sidebar').addEventListener('click', async e => {
    const act = e.target.closest('[data-act]');
    if (!act) return;
    if (act.dataset.act === 'proj') {
      const id = parseInt(act.dataset.id, 10);
      const isFolder = act.dataset.folder === '1';
      if (isFolder) {
        const key = 'p' + id;
        if (S.expanded.has(key)) S.expanded.delete(key); else S.expanded.add(key);
        renderProjectTree();
        return;
      }
      S.selectedProject = id;
      S.selectedTag = null;
      renderProjectTree();
      await switchView('projects');
    } else if (act.dataset.act === 'tag') {
      S.selectedTag = act.dataset.name;
      S.selectedProject = null;
      await switchView('tags');
    } else if (act.dataset.act === 'add-project') {
      const name = prompt('新建项目名称：');
      if (name && name.trim()) {
        try {
          await api('POST', '/api/projects', { name: name.trim() });
          toast('项目已创建');
          await refreshAll();
        } catch (err) { toast(err.message, 'err'); }
      }
    }
  });

  // 内容区事件委托
  $('#content').addEventListener('click', async e => {
    const act = e.target.closest('[data-act]');
    if (!act) return;
    const a = act.dataset.act;
    const id = act.dataset.id ? parseInt(act.dataset.id, 10) : null;

    try {
      if (a === 'done') {
        const r = await api('POST', '/api/tasks/' + id + '/complete');
        if (r.nextInstance && r.nextInstance.id) {
          toast('已完成任务，自动生成下一次：' + fmtDate(r.nextInstance.dueDate) + '（' + r.nextInstance.title + '）', 'ok');
        } else {
          toast('已完成', 'ok');
        }
        await refreshAll();
      } else if (a === 'reopen') {
        await api('POST', '/api/tasks/' + id + '/reopen');
        toast('已恢复');
        await refreshAll();
      } else if (a === 'edit') {
        let t = S.taskIndex.get(id);
        if (!t) {
          try {
            t = await apiGet('/api/tasks/' + id);
            S.taskIndex.set(t.id, t);
          } catch (e2) { toast('任务不存在或已删除', 'err'); return; }
        }
        await openTaskEditor(t);
      } else if (a === 'del') {
        if (confirm('确定删除该任务？（移入回收站，30 天内可在回收站恢复）')) {
          var delRow = act.closest('.task-row');
          if (delRow) { delRow.classList.add('is-deleting'); await new Promise(r => setTimeout(r, 250)); }
          await api('DELETE', '/api/tasks/' + id);
          toast('已移入回收站');
          await refreshAll();
        }
      } else if (a === 'addsub') {
        await openTaskEditor(Object.assign({}, S.taskIndex.get(id), { parentId: id }));
      } else if (a === 'tag') {
        S.selectedTag = act.dataset.name;
        S.selectedProject = null;
        await switchView('tags');
      } else if (a === 'proj') {
        const pid = parseInt(act.dataset.id, 10);
        const isFolder = act.dataset.folder === '1';
        if (isFolder) {
          const key = 'p' + pid;
          if (S.expanded.has(key)) S.expanded.delete(key); else S.expanded.add(key);
          renderProjectTree();
          await switchView('projects');
          return;
        }
        S.selectedProject = pid;
        S.selectedTag = null;
        renderProjectTree();
        await switchView('projects');
      } else if (a === 'hm-prev') {
        S.heatYear = (S.heatYear || parseInt(S.today.slice(0, 4), 10)) - 1;
        await switchView('stats');
      } else if (a === 'hm-next') {
        S.heatYear = (S.heatYear || parseInt(S.today.slice(0, 4), 10)) + 1;
        await switchView('stats');
      } else if (a === 'cal-prev') {
        if (S.calMode === 'week') {
          S.weekOffset--;
        } else {
          S.calMonth--;
          if (S.calMonth < 1) { S.calMonth = 12; S.calYear--; }
        }
        await switchView('calendar');
      } else if (a === 'cal-next') {
        if (S.calMode === 'week') {
          S.weekOffset++;
        } else {
          S.calMonth++;
          if (S.calMonth > 12) { S.calMonth = 1; S.calYear++; }
        }
        await switchView('calendar');
      } else if (a === 'cal-today') {
        const [y, m] = S.today.split('-').map(Number);
        S.calYear = y; S.calMonth = m; S.weekOffset = 0;
        await switchView('calendar');
      } else if (a === 'cal-mode') {
        S.calMode = act.dataset.mode || 'month';
        S.weekOffset = 0;
        await switchView('calendar');
      } else if (a === 'cal-holidays') {
        const year = S.calMode === 'week' ? parseInt(S.today.slice(0, 4), 10) : S.calYear;
        if (!confirm('将为 ' + year + ' 年生成法定节假日（春节/清明/端午/中秋等，已存在的跳过）。确认？')) return;
        const r = await api('POST', '/api/holidays/auto?year=' + year);
        toast('已生成 ' + r.year + ' 年 ' + r.added + ' 天节假日', 'ok');
        await switchView('calendar');
      } else if (a === 'cal-day') {
        await openDayModal(act.dataset.date);
      } else if (a === 'new') {
        await openTaskEditor(null);
      } else if (a === 'batch-toggle') {
        S.batchMode = !S.batchMode;
        if (!S.batchMode) S.batchSel.clear();
        await switchView(S.view);
      } else if (a === 'open-pomo') {
        showPomodoro();
      } else if (a === 'batch-go') {
        const op = act.dataset.op;
        if (op === 'delete') {
          if (!confirm('将 ' + S.batchSel.size + ' 项任务移入回收站？')) return;
        }
        var restore = btnLoading(act);
        try { await runBatch(op); } finally { restore(); }
      } else if (a === 'batch-tag') {
        const name = $('#batch-tag').value.trim();
        if (!name) { toast('请输入标签名', 'err'); return; }
        await runBatch('tag', { tag: name, remove: false });
      } else if (a === 'batch-clear') {
        S.batchSel.clear();
        await switchView(S.view);
      } else if (a === 'pomo') {
        showPomodoro(id);
      } else if (a === 'focus') {
        await enterFocusMode(id);
      } else if (a === 'day-prev') {
        S.dayDate = isoAddDays(S.dayDate || S.today, -1);
        await switchView('day');
      } else if (a === 'day-next') {
        S.dayDate = isoAddDays(S.dayDate || S.today, 1);
        await switchView('day');
      } else if (a === 'day-today') {
        S.dayDate = '';
        await switchView('day');
      } else if (a === 'export-go') {
        window.open('/api/export?format=' + act.dataset.fmt, '_blank');
        toast('已开始下载');
      } else if (a === 'trash-restore') {
        await api('POST', '/api/tasks/' + id + '/restore');
        toast('已恢复');
        await refreshAll();
      } else if (a === 'trash-purge') {
        if (confirm('彻底删除该任务？此操作不可恢复！')) {
          await api('DELETE', '/api/tasks/' + id + '?purge=1');
          toast('已彻底删除');
          await switchView('trash');
        }
      } else if (a === 'trash-clear') {
        if (confirm('清空回收站？所有已删除任务将被彻底移除，不可恢复！')) {
          await api('DELETE', '/api/trash');
          toast('回收站已清空');
          await switchView('trash');
        }
      } else if (a === 'save-filter') {
        const name = $('#f-name').value.trim();
        if (!name) { toast('请输入筛选名称', 'err'); return; }
        const spec = {
          tag: $('#f-tag').value,
          priority: $('#f-prio').value === '' ? '' : parseInt($('#f-prio').value, 10),
          dueWithin: parseInt($('#f-within').value, 10) || 0,
          status: $('#f-status').value
        };
        await api('POST', '/api/filters', { name: name, spec: spec });
        toast('筛选已保存');
        await switchView('filters');
      } else if (a === 'run-filter') {
        const f = S.filters.find(x => x.id === id);
        if (f) await runFilter(f);
      } else if (a === 'del-filter') {
        if (confirm('删除该筛选？')) {
          await api('DELETE', '/api/filters/' + id);
          toast('已删除');
          await switchView('filters');
        }
      } else if (a === 'tab-edit' || a === 'tab-prev') {
        /* 弹窗内事件已由 form 捕获 */
      }
    } catch (err) {
      toast(err.message, 'err');
    }
  });

  // 批量选择复选框
  $('#content').addEventListener('change', e => {
    if (e.target.classList && e.target.classList.contains('t-sel')) {
      const id = parseInt(e.target.dataset.id, 10);
      if (e.target.checked) S.batchSel.add(id);
      else S.batchSel.delete(id);
      const bar = $('#content .batch-bar .bb-count');
      if (bar) bar.textContent = S.batchSel.size;
    }
  });

  // 搜索
  const si = $('#search-input');
  const drop = $('#search-drop');
  si.addEventListener('input', () => {
    clearTimeout(S.searchTimer);
    const q = si.value.trim();
    if (!q) { drop.classList.add('hidden'); drop.innerHTML = ''; return; }
    S.searchTimer = setTimeout(async () => {
      try {
        const r = await apiGet('/api/search?q=' + encodeURIComponent(q));
        const list = r.tasks || [];
        drop.innerHTML = list.length
          ? list.slice(0, 12).map(t =>
              '<div class="s-item" data-act="edit" data-id="' + t.id + '">' +
              '<div class="s-title">' + esc(t.title) + '</div>' +
              '<div class="s-meta">' + (t.dueDate ? fmtDate(t.dueDate) : '无日期') +
              (t.project ? ' · ' + esc(t.project.name) : '') +
              (t.status === 'done' ? ' · 已完成' : '') + '</div></div>').join('')
          : '<div class="s-item" style="color:var(--text-3)">未找到匹配任务</div>';
        drop.classList.remove('hidden');
      } catch (e) { /* 忽略 */ }
    }, 260);
  });
  drop.addEventListener('click', async e => {
    const item = e.target.closest('[data-act]');
    if (!item) return;
    drop.classList.add('hidden');
    si.value = '';
    const t = S.taskIndex.get(parseInt(item.dataset.id, 10));
    if (!t) {
      // 从详情拉取
      const full = await apiGet('/api/tasks/' + item.dataset.id);
      S.taskIndex.set(full.id, full);
    }
    await openTaskEditor(S.taskIndex.get(item.dataset.id));
  });
  document.addEventListener('click', e => {
    if (!e.target.closest('.search-wrap')) drop.classList.add('hidden');
  });
  document.addEventListener('keydown', e => {
    if (e.key === 'Escape') drop.classList.add('hidden');
  });

  // 模态框内点击关闭；同步入口（导出/导入弹窗内）
  $('#modal-root').addEventListener('click', e => {
    const c = e.target.closest('[data-act="close-modal"]');
    if (c) closeModal();
    const s = e.target.closest('[data-act="sync-open"]');
    if (s) { closeModal(); openSyncModal(); }
  });
}

/* ---------------- 启动 ---------------- */
document.addEventListener('DOMContentLoaded', init);
