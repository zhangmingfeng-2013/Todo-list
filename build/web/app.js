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
  t.textContent = msg;
  $('#toast-root').appendChild(t);
  setTimeout(() => { t.style.opacity = '0'; t.style.transition = 'opacity .3s'; }, 2600);
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
  selectedProject: null, // 项目视图当前选中（0=未分类）
  selectedTag: null,
  expanded: new Set(),   // 项目视图展开的文件夹
  taskIndex: new Map(),  // id → 任务（供下拉选择）
  searchTimer: null
};

/* ---------------- 初始化 ---------------- */
async function init() {
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
  await refreshSidebar();
  await switchView('today');
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
async function switchView(v) {
  S.view = v;
  $$('#view-nav .nav-item').forEach(b => b.classList.toggle('active', b.dataset.view === v));
  const content = $('#content');
  content.innerHTML = '';
  try {
    if (v === 'today')     await renderToday(content);
    if (v === 'projects')  await renderProjects(content);
    if (v === 'tags')      await renderTags(content);
    if (v === 'calendar')  await renderCalendar(content);
    if (v === 'kanban')    await renderKanban(content);
    if (v === 'filters')   await renderFilters(content);
  } catch (e) {
    content.innerHTML = '<div class="loading">加载失败：' + esc(e.message) + '</div>';
  }
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
    '<button class="t-btn" data-act="addsub" data-id="' + t.id + '" title="添加子任务">＋</button>' +
    '<button class="t-btn" data-act="edit" data-id="' + t.id + '" title="编辑">✎</button>' +
    '<button class="t-btn danger" data-act="del" data-id="' + t.id + '" title="删除">🗑</button>' +
    '</div>';

  let notesPreview = '';
  if (t.notes) {
    const plain = t.notes.replace(/[#*`>_\[\]~-]/g, '').replace(/\s+/g, ' ').trim();
    if (plain) notesPreview = '<div class="t-notes-preview">' + esc(plain.slice(0, 80)) + '</div>';
  }

  const subCount = (t.children && t.children.length) ? '<span class="m-chip">子任务 ' + t.children.length + '</span>' : '';

  return '<div class="task-row status-' + esc(t.status) + ' depth-' + Math.min(d, 6) +
    (t.blocked ? ' blocked' : '') + '" data-id="' + t.id + '">' +
    '<div class="t-check" data-act="' + (done ? 'reopen' : 'done') + '" data-id="' + t.id + '">' + checkIco + '</div>' +
    '<div class="t-main">' +
    '<div class="t-title">' + esc(t.title) + '</div>' +
    notesPreview +
    '<div class="t-meta">' + taskMeta(t) + subCount + '</div>' +
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
  const data = await apiGet('/api/today');
  const groups = [
    { key: 'overdue',   title: '已逾期',   badge: 'over', cls: 'b-red',   none: '没有逾期任务' },
    { key: 'dueToday',  title: '今日到期', badge: '',      cls: 'b-blue',  none: '今日无到期任务' },
    { key: 'startToday', title: '今日开始', badge: '',     cls: 'b-green', none: '今日无开始任务' },
    { key: 'noDate',    title: '无日期',   badge: '',      cls: '',        none: '无日期任务都清空了' },
    { key: 'doneToday', title: '今日已完成', badge: data.doneToday.length, cls: 'b-green', none: '今天还没完成任务', muted: true }
  ];
  const head = viewHead('今日',
    S.today ? (S.today.slice(0, 4) + '年' + parseInt(S.today.slice(5, 7), 10) + '月' +
      parseInt(S.today.slice(8, 10), 10) + '日 · 农历 ' + S.lunarToday) : '');
  content.appendChild(head);

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

/* ---------------- 日历视图 ---------------- */
const CAL_DOW = ['一', '二', '三', '四', '五', '六', '日'];

async function renderCalendar(content) {
  const head = viewHead('日历', '含农历与节假日标注');
  content.appendChild(head);

  const toolbar = document.createElement('div');
  toolbar.className = 'cal-toolbar';
  toolbar.innerHTML =
    '<button class="btn btn-sm" data-act="cal-prev">‹ 上月</button>' +
    '<span class="month-title">' + S.calYear + '年' + S.calMonth + '月</span>' +
    '<button class="btn btn-sm" data-act="cal-next">下月 ›</button>' +
    '<button class="btn btn-sm" data-act="cal-today">回到今天</button>' +
    '<div class="spacer" style="flex:1"></div>' +
    '<button class="btn btn-sm" data-act="new" title="在本月新建任务">＋ 新建</button>';
  content.appendChild(toolbar);

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

/* ---------------- 模态框 ---------------- */
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
  $('#modal-root').innerHTML = '';
  if (modalKeyHandler) {
    document.removeEventListener('keydown', modalKeyHandler);
    modalKeyHandler = null;
  }
}

/* ---------------- 任务编辑器 ---------------- */
async function openTaskEditor(task) {
  const editing = !!task;
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
  const btnSave = document.createElement('button');
  btnSave.className = 'btn btn-primary';
  btnSave.textContent = editing ? '保存修改' : '创建任务';
  foot.appendChild(btnCancel);
  foot.appendChild(btnSave);

  openModal(editing ? ('编辑任务 #' + t.id) : '新建任务', body, foot);

  btnSave.addEventListener('click', async () => {
    const payload = collectTask(t);
    if (!payload.title) { toast('标题不能为空', 'err'); return; }
    try {
      if (editing) {
        await api('PUT', '/api/tasks/' + t.id, payload);
        toast('已保存');
      } else {
        const r = await api('POST', '/api/tasks', payload);
        toast('已创建 #' + r.task.id);
      }
      closeModal();
      await refreshAll();
    } catch (err) { toast('保存失败：' + err.message, 'err'); }
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

  btnGo.addEventListener('click', async () => {
    const format = $('#imp-format').value;
    const text = $('#imp-text').value.trim();
    if (!text) { toast('请粘贴内容', 'err'); return; }
    btnGo.disabled = true;
    btnGo.textContent = '导入中…';
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
        toast('导入完成');
        setTimeout(() => { closeModal(); refreshAll(); }, 1500);
      } else {
        btnGo.disabled = false;
        btnGo.textContent = '重新导入';
      }
    } catch (e) {
      toast('导入失败：' + e.message, 'err');
      btnGo.disabled = false;
      btnGo.textContent = '开始导入';
    }
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
    '</div>';

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
  // 视图导航
  $('#view-nav').addEventListener('click', e => {
    const b = e.target.closest('.nav-item');
    if (b) switchView(b.dataset.view);
  });

  // 新建 / 导入 / 存储
  $('#btn-new').addEventListener('click', () => openTaskEditor(null));
  $('#btn-import').addEventListener('click', openImportModal);
  $('#btn-storage').addEventListener('click', openStorageModal);

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
        await openTaskEditor(S.taskIndex.get(id));
      } else if (a === 'del') {
        if (confirm('确定删除该任务？（子任务一并删除）')) {
          await api('DELETE', '/api/tasks/' + id);
          toast('已删除');
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
      } else if (a === 'cal-prev') {
        S.calMonth--;
        if (S.calMonth < 1) { S.calMonth = 12; S.calYear--; }
        await switchView('calendar');
      } else if (a === 'cal-next') {
        S.calMonth++;
        if (S.calMonth > 12) { S.calMonth = 1; S.calYear++; }
        await switchView('calendar');
      } else if (a === 'cal-today') {
        const [y, m] = S.today.split('-').map(Number);
        S.calYear = y; S.calMonth = m;
        await switchView('calendar');
      } else if (a === 'cal-day') {
        await openDayModal(act.dataset.date);
      } else if (a === 'new') {
        await openTaskEditor(null);
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

  // 模态框内点击关闭
  $('#modal-root').addEventListener('click', e => {
    const c = e.target.closest('[data-act="close-modal"]');
    if (c) closeModal();
  });
}

/* ---------------- 启动 ---------------- */
document.addEventListener('DOMContentLoaded', init);
