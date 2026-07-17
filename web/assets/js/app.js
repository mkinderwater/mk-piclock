'use strict';

const host = document.querySelector('#module-host');
const noticeNode = document.querySelector('#action-notice');
const oledPill = document.querySelector('#oled-pill');
const clockNameHeading = document.querySelector('#clock-name-heading');
const menus = [document.querySelector('#mobile-menu'), document.querySelector('#sidebar-menu')];
const authGate = document.querySelector('#auth-gate');
const authForm = document.querySelector('#auth-form');
const authInput = document.querySelector('#auth-password');
const authError = document.querySelector('#auth-error');
const authSubmit = document.querySelector('#auth-submit');

let modules = [];
let current = null;
let status = null;
let appBooted = false;
let authPromise = null;
let authResolve = null;
let noticeTimer = null;
const statusListeners = new Set();
const GUI_VERSION = '1.8.0';
const GUI_ASSET_VERSION = GUI_VERSION;
const REQUIRED_API_VERSION = '1.25';
const oledPreviewIntensity = Array.from({length: 16}, (_, level) =>
    level === 0 ? 0 : Math.pow(level / 15, 0.48));
const oledPreviewColours = Object.freeze({
    yellow: [255, 216, 74],
    green: [157, 255, 118],
    white: [245, 247, 255]
});

function createOledPreview(canvas) {
    const context = canvas.getContext('2d', {alpha: false});
    context.imageSmoothingEnabled = false;
    let colour = oledPreviewColours.green;
    let lastBuffer = null;

    const clear = () => {
        lastBuffer = null;
        context.fillStyle = '#000';
        context.fillRect(0, 0, canvas.width, canvas.height);
    };

    const draw = buffer => {
        const bytes = new Uint8Array(buffer);
        if (bytes.length !== 256 * 64 / 2) throw new Error('Invalid OLED framebuffer size');
        lastBuffer = buffer.slice(0);
        const image = context.createImageData(256, 64);
        let pixel = 0;
        for (const packed of bytes) {
            for (const level of [(packed >> 4) & 0x0f, packed & 0x0f]) {
                const intensity = oledPreviewIntensity[level];
                const offset = pixel++ * 4;
                image.data[offset] = Math.round(colour[0] * intensity);
                image.data[offset + 1] = Math.round(colour[1] * intensity);
                image.data[offset + 2] = Math.round(colour[2] * intensity);
                image.data[offset + 3] = 255;
            }
        }
        context.putImageData(image, 0, 0);
    };

    const setColour = name => {
        colour = oledPreviewColours[String(name || '').toLowerCase()] || oledPreviewColours.green;
        if (lastBuffer) draw(lastBuffer);
    };

    clear();
    return Object.freeze({clear, draw, setColour});
}

const oledThemes = Object.freeze({
    yellow: {color: '#ffd84a', faint: 'rgba(255,216,74,.38)', border: 'rgba(255,216,74,.35)', divider: 'rgba(255,216,74,.14)', glow: 'rgba(255,216,74,.32)'},
    green: {color: '#9dff76', faint: 'rgba(157,255,118,.38)', border: 'rgba(157,255,118,.35)', divider: 'rgba(157,255,118,.14)', glow: 'rgba(157,255,118,.32)'},
    white: {color: '#f5f7ff', faint: 'rgba(245,247,255,.38)', border: 'rgba(245,247,255,.35)', divider: 'rgba(245,247,255,.14)', glow: 'rgba(245,247,255,.32)'}
});

function applyOledTheme(name) {
    const theme = oledThemes[String(name || '').toLowerCase()] || oledThemes.green;
    const style = document.documentElement.style;
    style.setProperty('--oled-color', theme.color);
    style.setProperty('--oled-color-faint', theme.faint);
    style.setProperty('--oled-color-border', theme.border);
    style.setProperty('--oled-color-divider', theme.divider);
    style.setProperty('--oled-color-glow', theme.glow);
}

async function refreshApiState() {
    const data = await json('/api/v1');
    if (!data.api_version || !data.product_version) {
        throw new Error(`The web GUI is v${GUI_VERSION}, but the running API is older. Rebuild, reinstall, and restart mk-piclock-api.`);
    }
    if (data.api_version !== REQUIRED_API_VERSION) {
        throw new Error(`The web GUI requires API v${REQUIRED_API_VERSION}, but v${data.api_version} is running. Rebuild, reinstall, and restart mk-piclock-api.`);
    }
    return data;
}

async function openControlPanel() {
    if (!appBooted) {
        const manifest = await json(`/modules/modules.json?v=${GUI_ASSET_VERSION}`);
        modules = (manifest.modules || [])
            .filter(item => item?.enabled === true && /^[a-z0-9_-]+$/i.test(item.id || ''))
            .sort((a, b) => (a.order || 0) - (b.order || 0));
        renderMenus();
        appBooted = true;
    }
    current = null;
    await refreshStatusNow();
    navigate(hashModule());
}

function escapeHtml(value) {
    return String(value ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

async function responseError(response) {
    let message = await response.text();
    try {
        const parsed = JSON.parse(message);
        message = parsed.error || parsed.message || message;
    } catch (_) {}
    return new Error(message || `HTTP ${response.status}`);
}

function setAuthError(text = '') {
    authError.textContent = text;
    authError.classList.toggle('hidden', !text);
}

function closeAuthGate() {
    authGate.classList.add('hidden');
    authInput.value = '';
    setAuthError();
}

function openAuthGate() {
    authGate.classList.remove('hidden');
    window.setTimeout(() => authInput.focus(), 0);
    if (!authPromise) {
        authPromise = new Promise(resolve => { authResolve = resolve; });
    }
    return authPromise;
}

async function ensureAuthenticated() {
    const response = await fetch('/api/v1/auth/status', {
        cache: 'no-store', credentials: 'same-origin', headers: {'Accept': 'application/json'}
    });
    if (!response.ok) throw await responseError(response);
    const state = await response.json();
    if (!state.password_required || state.authenticated) {
        closeAuthGate();
        return true;
    }
    return openAuthGate();
}

async function request(url, options = {}) {
    const {authRetry = true, ...fetchOptions} = options;
    const response = await fetch(url, {
        ...fetchOptions,
        cache: 'no-store',
        credentials: 'same-origin'
    });
    if (response.ok) return response;
    if (response.status === 401 && authRetry &&
        url !== '/api/v1/auth/login' && url !== '/api/v1/auth/status') {
        await openAuthGate();
        return request(url, {...fetchOptions, authRetry: false});
    }
    throw await responseError(response);
}

async function json(url, options) {
    return (await request(url, options)).json();
}

async function binary(url, options) {
    return (await request(url, options)).arrayBuffer();
}

function notice(text = '', type = '', timeout = 0) {
    window.clearTimeout(noticeTimer);
    noticeTimer = null;
    noticeNode.textContent = text;
    noticeNode.className = `action-notice ${type}`;
    noticeNode.classList.toggle('hidden', !text);
    if (timeout) {
        noticeTimer = window.setTimeout(() => {
            if (noticeNode.textContent === text) noticeNode.classList.add('hidden');
        }, timeout);
    }
}

function busy(button, active, text = '') {
    if (!button) return;
    if (active) {
        button.dataset.label = button.textContent;
        button.disabled = true;
        if (text) button.textContent = text;
        return;
    }
    button.disabled = false;
    if (button.dataset.label) button.textContent = button.dataset.label;
    delete button.dataset.label;
}

async function update(url, options = {}) {
    const {
        method = 'GET', body = null, button = null,
        busyText = 'Processing request...', done = 'Request completed', errorText = 'Request failed',
        refreshStatus = true, doneTimeout = 1800, errorTimeout = 3500
    } = options;

    busy(button, true, busyText);
    notice(busyText, 'busy');
    try {
        const headers = body instanceof URLSearchParams
            ? {'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json'}
            : {'Accept': 'application/json'};
        const response = await request(url, {method, body, headers});
        notice(done, 'ok', doneTimeout);
        if (refreshStatus) await refreshStatusNow();
        return response;
    } catch (error) {
        notice(error.message || errorText, 'warn', errorTimeout);
        throw error;
    } finally {
        busy(button, false);
    }
}

function clock(action, params = {}, button = null, feedback = {}) {
    const messages = {
        clock: {busyText: 'Restoring clock...', done: 'Clock display restored'},
        clear: {busyText: 'Clearing screen...', done: 'Screen cleared'},
        stop: {busyText: 'Stopping audio...', done: 'Audio stopped'},
        'play-music': {busyText: 'Starting music...', done: 'Music playback started'}
    };
    const defaults = messages[action] || {busyText: 'Sending clock command...', done: 'Clock command completed'};
    const body = new URLSearchParams({do: action, format: 'json', ...params});
    return update('/api/v1/display/action', {
        method: 'POST',
        body,
        button,
        busyText: feedback.busyText || defaults.busyText,
        done: feedback.done || defaults.done,
        errorText: feedback.errorText || 'Clock command failed'
    });
}

function formatUptime(value) {
    let seconds = Math.max(0, Number.parseInt(value || 0, 10));
    const days = Math.floor(seconds / 86400); seconds %= 86400;
    const hours = Math.floor(seconds / 3600); seconds %= 3600;
    const minutes = Math.floor(seconds / 60); seconds %= 60;
    if (days) return `${days}d ${hours}h ${minutes}m`;
    if (hours) return `${hours}h ${minutes}m ${seconds}s`;
    if (minutes) return `${minutes}m ${seconds}s`;
    return `${seconds}s`;
}

function timeValue(hour, minute) {
    return `${String(hour).padStart(2, '0')}:${String(minute).padStart(2, '0')}`;
}

function subscribeStatus(callback, signal) {
    statusListeners.add(callback);
    if (status) callback(status);
    signal?.addEventListener('abort', () => statusListeners.delete(callback), {once: true});
}

async function refreshStatusNow() {
    try {
        status = await json('/api/v1/status');
        oledPill.textContent = status.oled_ok ? 'Clock connected' : 'Screen unavailable';
        oledPill.className = status.oled_ok ? 'pill ok' : 'pill warn';
        applyOledTheme(status.oled_color);
        const clockName = String(status.clock_name || '').trim() || 'mk-piclock';
        clockNameHeading.textContent = clockName;
        clockNameHeading.title = `Editing ${clockName}`;
        if (current) {
            const activeDefinition = definitionFor(current.id);
            document.title = `${activeDefinition?.name || 'Control'} | ${clockName}`;
        }
        statusListeners.forEach(callback => {
            try { callback(status); } catch (error) { console.error(error); }
        });
        return status;
    } catch (_) {
        oledPill.textContent = 'Clock offline';
        oledPill.className = 'pill warn';
        return null;
    }
}

function hashModule() {
    return location.hash.replace(/^#\/?/, '').split(/[/?]/)[0].trim();
}

function definitionFor(id) {
    return modules.find(item => item.id === id)
        || modules.find(item => item.default)
        || modules[0];
}

function navigate(id, replace = false) {
    const definition = definitionFor(id);
    if (!definition) return;

    const targetHash = `#${definition.id}`;
    if (location.hash !== targetHash) {
        const method = replace ? 'replaceState' : 'pushState';
        history[method](null, '', targetHash);
    }

    /* Load immediately. Do not depend on a later hashchange event, which made
       some browsers require a second menu click before the module appeared. */
    void loadModule(definition.id);
}

function makeMenuButton(menu, definition) {
    const button = document.createElement('button');
    button.type = 'button';
    button.className = menu.id === 'mobile-menu' ? 'nav-item mobile-nav-item' : 'nav-item';
    button.dataset.moduleId = definition.id;
    button.dataset.moduleGroup = definition.group || '';
    button.textContent = definition.name;
    button.addEventListener('click', () => navigate(definition.id));
    return button;
}

function renderMenus() {
    menus.forEach(menu => {
        if (menu.id === 'mobile-menu') {
            menu.replaceChildren(...modules.map(definition => makeMenuButton(menu, definition)));
            return;
        }
        const nodes = [];
        let group = '';
        for (const definition of modules) {
            if (definition.group !== group) {
                group = definition.group || '';
                const heading = document.createElement('div');
                heading.className = 'sidebar-title';
                heading.textContent = group;
                nodes.push(heading);
            }
            nodes.push(makeMenuButton(menu, definition));
        }
        menu.replaceChildren(...nodes);
    });
}

function setActiveMenu(id) {
    document.querySelectorAll('[data-module-id]').forEach(button => {
        button.classList.toggle('active', button.dataset.moduleId === id);
    });
}

function moduleContext(controller) {
    const $ = selector => host.querySelector(selector);
    const $$ = selector => [...host.querySelectorAll(selector)];
    const set = (selector, property, value) => {
        const node = $(selector);
        if (node) node[property] = value == null ? '' : String(value);
    };
    const on = (type, selector, handler, target = host) => {
        target.addEventListener(type, event => {
            const match = event.target.closest(selector);
            if (match && target.contains(match)) handler(event, match);
        }, {signal: controller.signal});
    };

    return Object.freeze({
        root: host,
        signal: controller.signal,
        $,
        $$,
        on,
        html: escapeHtml,
        setText: (selector, value) => set(selector, 'textContent', value),
        setValue: (selector, value) => set(selector, 'value', value),
        json,
        binary,
        createOledPreview,
        update,
        clock,
        notice,
        busy,
        formatUptime,
        timeValue,
        status: Object.freeze({
            get: () => status,
            refresh: refreshStatusNow,
            subscribe: callback => subscribeStatus(callback, controller.signal)
        })
    });
}

async function loadModule(id) {
    const definition = definitionFor(id);
    if (!definition || current?.id === definition.id) return;

    current?.controller.abort();
    current?.css.remove();

    const controller = new AbortController();
    const base = `/modules/${encodeURIComponent(definition.id)}`;
    const css = document.createElement('link');
    css.rel = 'stylesheet';
    css.href = `${base}/module.css?v=${GUI_ASSET_VERSION}`;
    document.head.appendChild(css);
    current = {id: definition.id, controller, css, refresh: null};

    setActiveMenu(definition.id);
    host.innerHTML = `<div class="card module-loading">Opening ${escapeHtml(definition.name)}...</div>`;

    try {
        const [html, moduleObject] = await Promise.all([
            request(`${base}/module.html?v=${GUI_ASSET_VERSION}`).then(response => response.text()),
            import(`${base}/module.js?v=${GUI_ASSET_VERSION}`)
        ]);
        if (controller.signal.aborted) return;

        host.innerHTML = html;
        document.title = `${definition.name} | ${String(status?.clock_name || '').trim() || 'mk-piclock'}`;
        const mounted = await moduleObject.mount?.(moduleContext(controller));
        if (!controller.signal.aborted) current.refresh = mounted?.refresh || null;
    } catch (error) {
        if (controller.signal.aborted) return;
        css.remove();
        current = null;
        host.innerHTML = `<div class="card module-error"><h2>This page could not open</h2><p>${escapeHtml(error.message)}</p></div>`;
        notice(`${definition.name} could not open`, 'warn', 3500);
    }
}

host.addEventListener('submit', async event => {
    const form = event.target.closest('form');
    if (!form || form.dataset.moduleHandlesSubmit === 'true') return;
    event.preventDefault();

    const multipart = form.enctype.includes('multipart/form-data');
    const body = multipart ? new FormData(form) : new URLSearchParams(new FormData(form));
    if (!multipart) body.set('format', 'json');
    const button = event.submitter || form.querySelector('[type="submit"]');

    try {
        await update(form.action, {
            method: (form.method || 'POST').toUpperCase(),
            body,
            button,
            busyText: form.dataset.busyText || (multipart ? 'Uploading files...' : 'Saving changes...'),
            done: form.dataset.successText || (multipart ? 'Files uploaded' : 'Changes saved'),
            errorText: form.dataset.errorText || (multipart ? 'Upload failed' : 'Changes could not be saved')
        });
        await current?.refresh?.();
    } catch (_) {}
});


authForm.addEventListener('submit', async event => {
    event.preventDefault();
    const password = authInput.value;
    busy(authSubmit, true, 'Checking...');
    setAuthError();
    try {
        await request('/api/v1/auth/login', {
            method: 'POST',
            body: new URLSearchParams({password}),
            headers: {'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json'},
            authRetry: false
        });
        closeAuthGate();
        const resolve = authResolve;
        authPromise = null;
        authResolve = null;
        resolve?.(true);
    } catch (error) {
        setAuthError(error.message || 'Password is incorrect');
        authInput.select();
    } finally {
        busy(authSubmit, false);
    }
});

async function start() {
    try {
        await ensureAuthenticated();
        await refreshApiState();
        await openControlPanel();
    } catch (error) {
        host.innerHTML = `<div class="card module-error"><h2>Clock controls could not open</h2><p>${escapeHtml(error.message)}</p></div>`;
        notice(error.message || 'Clock API unavailable.', 'warn');
    }
}

const loadHistoryModule = () => void loadModule(hashModule());
window.addEventListener('hashchange', loadHistoryModule);
window.addEventListener('popstate', loadHistoryModule);
window.addEventListener('beforeunload', () => current?.controller.abort());

start();
