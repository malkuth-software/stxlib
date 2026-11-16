// app.js: the appliance's web interface (C2). One page; each section is
// built from the JSON interface (runtime/web/api.h). Text from the
// appliance is written with textContent only, never as HTML. Functions are
// named (no arrow functions), after the house style's no-lambda rule.
'use strict';

const state = {
  csrf: null,
  page: null,
  rebooting: false,
  logTarget: null,
};

const pages = ['status', 'targets', 'updates', 'network', 'access', 'settings', 'reboot'];

// ---------------------------------------------------------------------------
// Small helpers.

function byId(id) {
  return document.getElementById(id);
}

function element(tag, className, text) {
  const node = document.createElement(tag);
  if (className) {
    node.className = className;
  }
  if (text !== undefined && text !== null) {
    node.textContent = String(text);
  }
  return node;
}

function clear(node) {
  while (node.firstChild) {
    node.removeChild(node.firstChild);
  }
}

function showNotice(text, isError) {
  const notice = byId('notice');
  notice.textContent = text;
  notice.className = isError ? 'notice error' : 'notice';
  notice.hidden = !text;
}

function errorText(result) {
  if (result.data && typeof result.data.error === 'string') {
    let text = result.data.error.replace(/_/g, ' ');
    if (typeof result.data.line === 'number' && result.data.line > 0) {
      text += ' (line ' + result.data.line + ', column ' + result.data.column + ')';
    }
    if (typeof result.data.reason === 'string') {
      text += ': ' + result.data.reason.replace(/_/g, ' ');
    }
    return text;
  }
  return 'request failed (HTTP ' + result.status + ')';
}

function factsInto(list, object) {
  clear(list);
  if (!object) {
    return;
  }
  const keys = Object.keys(object);
  for (let index = 0; index < keys.length; index += 1) {
    const value = object[keys[index]];
    if (value !== null && typeof value === 'object') {
      continue;
    }
    list.appendChild(element('dt', null, keys[index].replace(/_/g, ' ')));
    list.appendChild(element('dd', null, value === null ? '-' : String(value)));
  }
}

function durationText(milliseconds) {
  if (typeof milliseconds !== 'number') {
    return '-';
  }
  const seconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) {
    return days + 'd ' + hours + 'h';
  }
  if (hours > 0) {
    return hours + 'h ' + minutes + 'm';
  }
  return minutes + 'm ' + (seconds % 60) + 's';
}

// ---------------------------------------------------------------------------
// The JSON interface.

async function api(method, path, body) {
  const options = { method: method, headers: {}, credentials: 'same-origin', cache: 'no-store' };
  if (method === 'POST') {
    options.headers['Content-Type'] = 'application/json';
    if (state.csrf) {
      options.headers['X-STX-CSRF'] = state.csrf;
    }
    options.body = JSON.stringify(body || {});
  }
  let response = null;
  try {
    response = await fetch(path, options);
  } catch (failure) {
    return { status: 0, data: { error: 'appliance_unreachable' } };
  }
  let data = null;
  try {
    data = await response.json();
  } catch (failure) {
    data = null;
  }
  if (response.status === 401 && path !== '/api/login' && path !== '/api/password') {
    state.csrf = null;
    showLogin();
  }
  return { status: response.status, data: data };
}

// ---------------------------------------------------------------------------
// Session.

function hideAllPages() {
  const sections = document.querySelectorAll('main > section');
  for (let index = 0; index < sections.length; index += 1) {
    sections[index].hidden = true;
  }
}

async function showLogin() {
  byId('nav').hidden = true;
  hideAllPages();
  const result = await api('GET', '/api/session');
  if (result.data && result.data.logged_in && typeof result.data.csrf === 'string') {
    state.csrf = result.data.csrf;
    startPages();
    return;
  }
  if (result.data && result.data.setup_required) {
    byId('page-setup').hidden = false;
    byId('setup-code').focus();
  } else {
    byId('page-login').hidden = false;
    byId('login-password').focus();
  }
}

async function onLogin(event) {
  event.preventDefault();
  const password = byId('login-password').value;
  const result = await api('POST', '/api/login', { password: password });
  byId('login-password').value = '';
  if (result.status === 200 && result.data && typeof result.data.csrf === 'string') {
    state.csrf = result.data.csrf;
    showNotice('', false);
    startPages();
    return;
  }
  if (result.status === 429) {
    showNotice('Too many wrong passwords: wait and try again.', true);
    return;
  }
  showNotice(errorText(result), true);
}

async function onSetup(event) {
  event.preventDefault();
  const password = byId('setup-password').value;
  if (password !== byId('setup-repeat').value) {
    showNotice('The two passwords differ.', true);
    return;
  }
  const result = await api('POST', '/api/setup', { code: byId('setup-code').value, password: password });
  byId('setup-password').value = '';
  byId('setup-repeat').value = '';
  if (result.status === 200 && result.data && typeof result.data.csrf === 'string') {
    state.csrf = result.data.csrf;
    showNotice('Password set.', false);
    startPages();
    return;
  }
  showNotice(errorText(result), true);
}

async function onLogout() {
  await api('POST', '/api/logout', {});
  state.csrf = null;
  showNotice('Logged out.', false);
  showLogin();
}

function startPages() {
  byId('page-login').hidden = true;
  byId('page-setup').hidden = true;
  byId('nav').hidden = false;
  showPage();
}

// ---------------------------------------------------------------------------
// Navigation.

function currentPage() {
  const name = window.location.hash.replace('#', '');
  return pages.indexOf(name) >= 0 ? name : 'status';
}

function showPage() {
  if (!state.csrf) {
    return;
  }
  const name = currentPage();
  state.page = name;
  hideAllPages();
  byId('page-' + name).hidden = false;
  const links = document.querySelectorAll('#nav a');
  for (let index = 0; index < links.length; index += 1) {
    links[index].className = links[index].getAttribute('href') === '#' + name ? 'current' : '';
  }
  const loaders = {
    status: loadStatus,
    targets: loadTargets,
    updates: loadOperation,
    network: loadNetwork,
    access: loadAccess,
    settings: loadSettings,
    reboot: null,
  };
  const loader = loaders[name];
  if (loader) {
    loader();
  }
}

// ---------------------------------------------------------------------------
// Status.

function serviceRow(table, record) {
  const row = element('tr');
  row.appendChild(element('td', null, record.name));
  const stateCell = element('td', 'state-' + record.state, record.state + (record.stopped_by_request ? ' (stopped)' : ''));
  row.appendChild(stateCell);
  row.appendChild(element('td', null, record.pid === null ? '-' : record.pid));
  row.appendChild(element('td', null, record.restart_count));
  row.appendChild(element('td', null, durationText(record.state_ms)));
  table.appendChild(row);
}

function serviceHeader(table) {
  const row = element('tr');
  const names = ['Service', 'State', 'PID', 'Restarts', 'For'];
  for (let index = 0; index < names.length; index += 1) {
    row.appendChild(element('th', null, names[index]));
  }
  table.appendChild(row);
}

async function loadStatus() {
  const supervisor = await api('GET', '/api/supervisor');
  if (supervisor.status === 200) {
    const facts = Object.assign({}, supervisor.data);
    facts.uptime = durationText(facts.uptime_ms);
    delete facts.uptime_ms;
    factsInto(byId('status-supervisor'), facts);
  } else {
    factsInto(byId('status-supervisor'), { error: errorText(supervisor) });
  }
  const slots = await api('GET', '/api/slots');
  factsInto(byId('status-slots'), slots.status === 200 ? slots.data : { error: errorText(slots) });
  const services = await api('GET', '/api/services');
  const table = byId('status-services');
  clear(table);
  if (services.status === 200 && services.data && Array.isArray(services.data.services)) {
    serviceHeader(table);
    for (let index = 0; index < services.data.services.length; index += 1) {
      serviceRow(table, services.data.services[index]);
    }
  }
  const log = await api('GET', '/api/boot-log');
  const logText = byId('status-log');
  if (log.status === 200 && log.data) {
    logText.textContent = (log.data.front_cut ? '...\n' : '') + (log.data.present ? log.data.text : '(no boot log)');
    logText.scrollTop = logText.scrollHeight;
  } else {
    logText.textContent = errorText(log);
  }
  const settings = await api('GET', '/api/settings');
  byId('pending-banner').hidden = !(settings.status === 200 && settings.data && settings.data.pending);
}

// ---------------------------------------------------------------------------
// Targets.

function settingsText(settings) {
  if (!settings) {
    return '';
  }
  const keys = Object.keys(settings);
  const lines = [];
  for (let index = 0; index < keys.length; index += 1) {
    lines.push(keys[index] + ' = ' + settings[keys[index]]);
  }
  return lines.join('\n');
}

function parseSettingsText(text) {
  const settings = {};
  const lines = text.split('\n');
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim();
    if (line === '' || line.charAt(0) === '#') {
      continue;
    }
    const equals = line.indexOf('=');
    if (equals <= 0) {
      return { error: 'line ' + (index + 1) + ' is not name = value' };
    }
    settings[line.slice(0, equals).trim()] = line.slice(equals + 1).trim();
  }
  return { settings: settings };
}

function actionButton(label, action, name, className) {
  const button = element('button', className || 'secondary', label);
  button.type = 'button';
  button.dataset.action = action;
  button.dataset.name = name;
  button.addEventListener('click', onTargetAction);
  return button;
}

function targetCard(target, programsSupported) {
  const card = element('div', 'target');
  card.appendChild(element('h3', null, target.name));
  const facts = element('dl', 'facts');
  const summary = {
    enabled_at_boot: target.enabled ? 'yes' : 'no',
    state: target.service ? target.service.state + (target.service.stopped_by_request ? ' (stopped)' : '') : 'not running (next boot)',
    restarts: target.service ? target.service.restart_count : '-',
  };
  if (target.program) {
    summary.active_slot = target.program.active;
    summary.version_a = target.program.version_a;
    summary.version_b = target.program.version_b;
    summary.on_trial = target.program.on_trial ? 'yes' : 'no';
    summary.update_available = target.program.update_available ? 'yes' : 'no';
  }
  factsInto(facts, summary);
  card.appendChild(facts);
  const settings = element('pre', 'log', settingsText(target.settings) || '(no settings)');
  card.appendChild(settings);
  const buttons = element('div', 'buttons');
  if (target.service) {
    buttons.appendChild(actionButton('Start', 'start', target.name));
    buttons.appendChild(actionButton('Stop', 'stop', target.name));
    buttons.appendChild(actionButton('Restart', 'restart', target.name));
  }
  buttons.appendChild(actionButton('Output', 'log', target.name));
  buttons.appendChild(actionButton('Edit', 'edit', target.name));
  if (programsSupported) {
    buttons.appendChild(actionButton('Install uploaded package', 'install-upload', target.name));
    buttons.appendChild(actionButton('Install from URL', 'install-url', target.name));
    buttons.appendChild(actionButton('Roll back', 'rollback', target.name));
    buttons.appendChild(actionButton('Check for update', 'check-update', target.name));
  }
  buttons.appendChild(actionButton('Delete', 'delete', target.name, 'danger'));
  card.appendChild(buttons);
  return card;
}

let targetsSeen = [];

async function loadTargets() {
  const result = await api('GET', '/api/targets');
  const list = byId('targets-list');
  clear(list);
  if (result.status !== 200 || !result.data) {
    list.appendChild(element('p', 'hint', errorText(result)));
    return;
  }
  targetsSeen = result.data.targets || [];
  if (result.data.pending) {
    list.appendChild(element('div', 'banner', 'Saved changes take effect at the next reboot.'));
  }
  if (targetsSeen.length === 0) {
    list.appendChild(element('p', 'hint', 'No targets yet.'));
  }
  for (let index = 0; index < targetsSeen.length; index += 1) {
    list.appendChild(targetCard(targetsSeen[index], result.data.programs_supported));
  }
  if (result.data.system_services && result.data.system_services.length > 0) {
    list.appendChild(element('h2', null, 'System services'));
    const table = element('table', 'grid');
    serviceHeader(table);
    for (let index = 0; index < result.data.system_services.length; index += 1) {
      serviceRow(table, result.data.system_services[index]);
    }
    list.appendChild(table);
  }
}

async function loadTargetLog() {
  const name = state.logTarget;
  if (!name) {
    return;
  }
  const box = byId('target-log-box');
  const text = byId('target-log');
  byId('target-log-title').textContent = 'Output of ' + name;
  box.hidden = false;
  const result = await api('GET', '/api/targets/' + encodeURIComponent(name) + '/log');
  if (result.status === 200 && result.data) {
    if (!result.data.present || result.data.text === '') {
      text.textContent = '(no output kept)';
    } else {
      text.textContent = (result.data.front_cut ? '...\n' : '') + result.data.text;
    }
    text.scrollTop = text.scrollHeight;
  } else {
    text.textContent = errorText(result);
  }
}

function closeTargetLog() {
  state.logTarget = null;
  byId('target-log-box').hidden = true;
}

function editTarget(name) {
  for (let index = 0; index < targetsSeen.length; index += 1) {
    if (targetsSeen[index].name === name) {
      byId('target-name').value = name;
      byId('target-settings').value = settingsText(targetsSeen[index].settings);
      byId('target-enabled').checked = targetsSeen[index].enabled;
      byId('target-form-title').textContent = 'Edit ' + name;
      byId('target-name').focus();
      return;
    }
  }
}

function clearTargetForm() {
  byId('target-name').value = '';
  byId('target-settings').value = '';
  byId('target-enabled').checked = true;
  byId('target-form-title').textContent = 'Add a target';
}

async function onTargetAction(event) {
  const action = event.currentTarget.dataset.action;
  const name = event.currentTarget.dataset.name;
  const path = encodeURIComponent(name);
  let result = null;
  if (action === 'edit') {
    editTarget(name);
    return;
  }
  if (action === 'log') {
    state.logTarget = name;
    loadTargetLog();
    return;
  }
  if (action === 'start' || action === 'stop' || action === 'restart') {
    result = await api('POST', '/api/services/' + path + '/' + action, {});
  } else if (action === 'delete') {
    if (!window.confirm('Delete the target ' + name + '? It stays running until the next reboot.')) {
      return;
    }
    result = await api('POST', '/api/targets/' + path + '/delete', {});
  } else if (action === 'install-upload') {
    result = await api('POST', '/api/targets/' + path + '/install', { source: 'upload' });
  } else if (action === 'install-url') {
    const url = window.prompt('Package URL (https://...)');
    if (!url) {
      return;
    }
    result = await api('POST', '/api/targets/' + path + '/install', { source: 'url', url: url });
  } else if (action === 'rollback') {
    result = await api('POST', '/api/targets/' + path + '/rollback', {});
  } else if (action === 'check-update') {
    result = await api('POST', '/api/targets/' + path + '/check-update', {});
  } else {
    return;
  }
  if (result.status === 200) {
    showNotice(name + ': ' + action + ' done.', false);
  } else {
    showNotice(name + ': ' + errorText(result), true);
  }
  loadTargets();
}

async function onTargetSave(event) {
  event.preventDefault();
  const name = byId('target-name').value.trim();
  const parsed = parseSettingsText(byId('target-settings').value);
  if (parsed.error) {
    showNotice(parsed.error, true);
    return;
  }
  const result = await api('POST', '/api/targets/' + encodeURIComponent(name),
    { settings: parsed.settings, enabled: byId('target-enabled').checked });
  if (result.status === 200) {
    showNotice('Target ' + name + ' saved; it takes effect at the next reboot.', false);
    clearTargetForm();
  } else {
    showNotice(errorText(result), true);
  }
  loadTargets();
}

// ---------------------------------------------------------------------------
// Updates.

function uploadFile(path, file) {
  const progress = byId('upload-progress');
  progress.hidden = false;
  progress.value = 0;
  const request = new XMLHttpRequest();
  request.open('POST', path);
  request.setRequestHeader('Content-Type', 'application/octet-stream');
  request.setRequestHeader('X-STX-CSRF', state.csrf || '');
  request.upload.addEventListener('progress', onUploadProgress);
  request.addEventListener('load', onUploadDone);
  request.addEventListener('error', onUploadFailed);
  request.send(file);
}

function onUploadProgress(event) {
  if (event.lengthComputable && event.total > 0) {
    byId('upload-progress').value = Math.floor((event.loaded * 100) / event.total);
  }
}

function onUploadDone(event) {
  const request = event.currentTarget;
  byId('upload-progress').hidden = true;
  let data = null;
  try {
    data = JSON.parse(request.responseText);
  } catch (failure) {
    data = null;
  }
  if (request.status === 200 && data) {
    showNotice('Uploaded ' + data.bytes + ' bytes, SHA-256 ' + data.sha256 + '.', false);
  } else {
    showNotice(errorText({ status: request.status, data: data }), true);
  }
}

function onUploadFailed() {
  byId('upload-progress').hidden = true;
  showNotice('The upload did not complete.', true);
}

function onUploadPackage(event) {
  event.preventDefault();
  const file = byId('upload-package-file').files[0];
  if (file) {
    uploadFile('/api/upload/package', file);
  }
}

function onUploadSystem(event) {
  event.preventDefault();
  const file = byId('upload-system-file').files[0];
  if (file) {
    uploadFile('/api/upload/system', file);
  }
}

async function onSystemUpdate() {
  if (!window.confirm('Install the uploaded system image to the other slot?')) {
    return;
  }
  const result = await api('POST', '/api/system/update', {});
  showNotice(result.status === 200 ? 'System update started.' : errorText(result), result.status !== 200);
  loadOperation();
}

async function loadOperation() {
  const result = await api('GET', '/api/operation');
  factsInto(byId('operation'), result.status === 200 ? result.data : { error: errorText(result) });
}

// ---------------------------------------------------------------------------
// Network.

async function loadNetwork() {
  const result = await api('GET', '/api/network');
  const table = byId('network-addresses');
  clear(table);
  if (result.status !== 200 || !result.data) {
    showNotice(errorText(result), true);
    return;
  }
  const header = element('tr');
  header.appendChild(element('th', null, 'Interface'));
  header.appendChild(element('th', null, 'Address'));
  table.appendChild(header);
  const addresses = result.data.addresses || [];
  for (let index = 0; index < addresses.length; index += 1) {
    const row = element('tr');
    row.appendChild(element('td', null, addresses[index].interface));
    row.appendChild(element('td', null, addresses[index].address + '/' + addresses[index].prefix));
    table.appendChild(row);
  }
  const settings = result.data.settings || {};
  byId('network-hostname').value = result.data.hostname || '';
  byId('network-mode').value = settings.mode === 'static' ? 'static' : 'dhcp';
  byId('network-address').value = settings.address || '';
  byId('network-gateway').value = settings.gateway || '';
  byId('network-dns').value = settings.dns || '';
}

async function onNetworkSave(event) {
  event.preventDefault();
  const body = { mode: byId('network-mode').value };
  const fields = ['hostname', 'address', 'gateway', 'dns'];
  for (let index = 0; index < fields.length; index += 1) {
    const value = byId('network-' + fields[index]).value.trim();
    body[fields[index]] = value === '' ? null : value;
  }
  const result = await api('POST', '/api/network', body);
  showNotice(result.status === 200 ? 'Network settings saved; they take effect at the next reboot.' : errorText(result),
    result.status !== 200);
}

// ---------------------------------------------------------------------------
// Access.

async function loadAccess() {
  const result = await api('GET', '/api/access');
  if (result.status === 200 && result.data) {
    byId('keys-text').value = (result.data.keys || []).join('\n');
  } else {
    showNotice(errorText(result), true);
  }
}

async function onPasswordChange(event) {
  event.preventDefault();
  const fresh = byId('password-new').value;
  if (fresh !== byId('password-repeat').value) {
    showNotice('The two new passwords differ.', true);
    return;
  }
  const result = await api('POST', '/api/password', { current: byId('password-current').value, new: fresh });
  byId('password-current').value = '';
  byId('password-new').value = '';
  byId('password-repeat').value = '';
  showNotice(result.status === 200 ? 'Password changed; other sessions were ended.' : errorText(result),
    result.status !== 200);
}

async function onKeysSave(event) {
  event.preventDefault();
  const keys = [];
  const lines = byId('keys-text').value.split('\n');
  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim();
    if (line !== '') {
      keys.push(line);
    }
  }
  const result = await api('POST', '/api/access', { keys: keys });
  showNotice(result.status === 200 ? 'SSH keys saved; they take effect at the next reboot.' : errorText(result),
    result.status !== 200);
}

// ---------------------------------------------------------------------------
// Settings.

let settingsSequence = 0;

async function loadSettings() {
  const result = await api('GET', '/api/settings');
  if (result.status === 200 && result.data) {
    settingsSequence = result.data.sequence;
    byId('settings-text').value = result.data.text;
    factsInto(byId('settings-facts'), {
      stored_in: result.data.source,
      turn: result.data.sequence,
      saved_since_boot: result.data.pending ? 'yes (reboot to apply)' : 'no',
    });
  } else {
    showNotice(errorText(result), true);
  }
  const factory = await api('GET', '/api/config');
  byId('settings-factory').textContent = factory.status === 200 && factory.data ? factory.data.text : errorText(factory);
}

async function onSettingsCheck() {
  const result = await api('POST', '/api/settings/check', { text: byId('settings-text').value });
  if (result.status === 200 && result.data && result.data.ok) {
    showNotice('The settings are valid.', false);
  } else {
    showNotice(errorText(result.data && !result.data.ok ? { status: result.status, data: {
      error: 'not valid', reason: result.data.reason, line: result.data.line, column: result.data.column } } : result), true);
  }
}

async function onSettingsSave() {
  const result = await api('POST', '/api/settings', { text: byId('settings-text').value, base_sequence: settingsSequence });
  if (result.status === 200) {
    showNotice('Settings saved; they take effect at the next reboot.', false);
    loadSettings();
  } else if (result.status === 409) {
    showNotice('The settings were changed elsewhere since this page read them: reload, then edit again.', true);
  } else {
    showNotice(errorText(result), true);
  }
}

// ---------------------------------------------------------------------------
// Reboot.

async function onReboot() {
  if (!window.confirm('Reboot the appliance now?')) {
    return;
  }
  const result = await api('POST', '/api/reboot', {});
  if (result.status !== 200) {
    showNotice(errorText(result), true);
    return;
  }
  state.rebooting = true;
  showNotice('Rebooting; this page reconnects when the appliance is back.', false);
  window.setTimeout(waitForReturn, 15000);
}

async function waitForReturn() {
  const result = await api('GET', '/api/session');
  if (result.status === 0) {
    window.setTimeout(waitForReturn, 3000);
    return;
  }
  state.rebooting = false;
  state.csrf = null;
  showNotice('The appliance is back; log in again.', false);
  showLogin();
}

// ---------------------------------------------------------------------------
// Start.

function start() {
  byId('login-form').addEventListener('submit', onLogin);
  byId('setup-form').addEventListener('submit', onSetup);
  byId('logout').addEventListener('click', onLogout);
  byId('status-refresh').addEventListener('click', loadStatus);
  byId('target-form').addEventListener('submit', onTargetSave);
  byId('target-clear').addEventListener('click', clearTargetForm);
  byId('target-log-refresh').addEventListener('click', loadTargetLog);
  byId('target-log-close').addEventListener('click', closeTargetLog);
  byId('upload-package-form').addEventListener('submit', onUploadPackage);
  byId('upload-system-form').addEventListener('submit', onUploadSystem);
  byId('system-update').addEventListener('click', onSystemUpdate);
  byId('network-form').addEventListener('submit', onNetworkSave);
  byId('password-form').addEventListener('submit', onPasswordChange);
  byId('keys-form').addEventListener('submit', onKeysSave);
  byId('settings-check').addEventListener('click', onSettingsCheck);
  byId('settings-save').addEventListener('click', onSettingsSave);
  byId('settings-reload').addEventListener('click', loadSettings);
  byId('reboot-button').addEventListener('click', onReboot);
  window.addEventListener('hashchange', showPage);
  showLogin();
}

document.addEventListener('DOMContentLoaded', start);
