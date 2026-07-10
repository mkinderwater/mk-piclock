export async function mount(ctx) {
    let fonts = null;
    const previewFonts = new Map();
    const fontBlobUrls = new Set();
    ctx.signal.addEventListener('abort', () => {
        fontBlobUrls.forEach(url => URL.revokeObjectURL(url));
        fontBlobUrls.clear();
    }, {once: true});
    let brightnessTimer = 0;
    let brightnessPending = null;
    let brightnessSending = false;

    const previewOledColour = value => {
        const themes = {
            yellow: ['#ffd84a', 'rgba(255,216,74,.38)', 'rgba(255,216,74,.35)', 'rgba(255,216,74,.14)', 'rgba(255,216,74,.32)'],
            green: ['#9dff76', 'rgba(157,255,118,.38)', 'rgba(157,255,118,.35)', 'rgba(157,255,118,.14)', 'rgba(157,255,118,.32)'],
            white: ['#f5f7ff', 'rgba(245,247,255,.38)', 'rgba(245,247,255,.35)', 'rgba(245,247,255,.14)', 'rgba(245,247,255,.32)']
        };
        const selected = themes[String(value || '').toLowerCase()] || themes.green;
        const style = document.documentElement.style;
        ['--oled-color', '--oled-color-faint', '--oled-color-border', '--oled-color-divider', '--oled-color-glow']
            .forEach((name, index) => style.setProperty(name, selected[index]));
    };

    const setBrightnessValue = value => {
        const percent = Math.max(0, Math.min(100, Number.parseInt(value ?? 35, 10) || 0));
        ctx.setValue('#bedtime-dim-percent', percent);
        ctx.setText('#bedtime-dim-value', `${percent}%`);
        ctx.setValue('#font-bedtime-dim-percent', percent);
        ctx.setValue('#advanced-bedtime-dim-percent', percent);
        return percent;
    };

    const applyStatus = status => {
        if (!status) return;
        const start = ctx.timeValue(status.bedtime_start_hour ?? 21, status.bedtime_start_min ?? 0);
        const end = ctx.timeValue(status.bedtime_end_hour ?? 7, status.bedtime_end_min ?? 0);
        const values = {
            '#clock-name': status.clock_name,
            '#bedtime-enabled': status.bedtime_enabled ? 1 : 0,
            '#bedtime-music-enabled': status.bedtime_music_enabled ? 1 : 0,
            '#bedtime-start': start,
            '#bedtime-end': end,
            '#clock-24h-mode': status.clock_24h_mode ? 1 : 0,
            '#oled-color': status.oled_color || 'green',
            '#bed-oled-font-file': status.oled_font_file,
            '#bed-oled-font': status.oled_font ?? 0,
            '#bed-oled-font-size': status.oled_font_size ?? 48,
            '#bed-clock-24h-mode': status.clock_24h_mode ? 1 : 0,
            '#font-bedtime-enabled': status.bedtime_enabled ? 1 : 0,
            '#font-bedtime-start': start,
            '#font-bedtime-end': end,
            '#advanced-bedtime-enabled': status.bedtime_enabled ? 1 : 0,
            '#advanced-bedtime-start': start,
            '#advanced-bedtime-end': end,
            '#advanced-clock-24h-mode': status.clock_24h_mode ? 1 : 0,
            '#oled-font-file': status.oled_font_file,
            '#oled-font': status.oled_font ?? 0,
            '#oled-font-size': status.oled_font_size ?? 48
        };
        Object.entries(values).forEach(([selector, value]) => ctx.setValue(selector, value));
        setBrightnessValue(status.bedtime_dim_percent ?? 35);
        previewOledColour(status.oled_color || 'green');
        ctx.setText('#startup-name-preview', status.clock_name);
        ctx.setText('#startup-version-preview', status.app_version);
    };

    const sendBrightnessPreview = async percent => {
        brightnessPending = percent;
        if (brightnessSending) return;
        brightnessSending = true;
        try {
            while (brightnessPending !== null && !ctx.signal.aborted) {
                const next = brightnessPending;
                brightnessPending = null;
                await ctx.json('/api/v1/display/brightness-preview', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/x-www-form-urlencoded', 'Accept': 'application/json'},
                    body: new URLSearchParams({brightness_percent: next, hold_seconds: 8, format: 'json'}),
                    signal: ctx.signal
                });
            }
        } catch (error) {
            if (!ctx.signal.aborted) ctx.notice(error.message || 'Brightness preview failed', 'warn', 2500);
        } finally {
            brightnessSending = false;
        }
    };

    const queueBrightnessPreview = percent => {
        window.clearTimeout(brightnessTimer);
        brightnessTimer = window.setTimeout(() => sendBrightnessPreview(percent), 90);
    };

    const fontFamily = name => `MKFont_${String(name).replace(/[^a-z0-9]/gi, '_')}`;
    const registerFont = name => {
        const family = fontFamily(name);
        if (!name || previewFonts.has(name)) return family;
        previewFonts.set(name, {family, loading: true});
        ctx.binary(`/api/v1/assets/fonts/file?file=${encodeURIComponent(name)}`, {signal: ctx.signal})
            .then(buffer => {
                if (ctx.signal.aborted) return;
                const blobUrl = URL.createObjectURL(new Blob([buffer], {type: 'font/ttf'}));
                fontBlobUrls.add(blobUrl);
                const font = new FontFace(family, `url(${blobUrl})`);
                previewFonts.set(name, {family, font, blobUrl});
                document.fonts.add(font);
                return font.load();
            })
            .catch(() => {});
        return family;
    };
    const sample = name => `<div class="font-sample-box" style="font-family:'${ctx.html(registerFont(name))}',sans-serif"><div>ABCDEFGHIJKLMNOPQRSTUVWXYZ</div><div>1234567890</div></div>`;

    const updatePreview = () => {
        if (!fonts) return;
        const selected = ctx.$('#advanced-oled-font-file').value;
        if (selected) {
            ctx.$('#selected-font-preview').innerHTML = `<div class="font-preview-card"><div class="font-name">${ctx.html(selected)}</div>${sample(selected)}</div>`;
            ctx.setText('#selected-font', selected);
            return;
        }
        const id = Number.parseInt(ctx.$('#advanced-oled-font').value || fonts.builtin || 0, 10);
        const name = fonts.builtin_fonts?.find(font => font.id === id)?.name || 'Built-in';
        ctx.$('#selected-font-preview').innerHTML = `<div class="font-preview-card"><div class="font-name">${ctx.html(name)}</div><div class="font-sample-box builtin-sample"><div>ABCDEFGHIJKLMNOPQRSTUVWXYZ</div><div>1234567890</div></div><div class="small muted">Built-in OLED font preview is approximated in the browser.</div></div>`;
        ctx.setText('#selected-font', name);
    };

    const loadFonts = async () => {
        fonts = await ctx.json('/api/v1/assets/fonts');
        ctx.setValue('#oled-font-size', fonts.font_size ?? 48);
        ctx.setValue('#advanced-oled-font-size', fonts.font_size ?? 48);
        ctx.$('#advanced-oled-font-file').innerHTML = [
            '<option value="">Use built-in font</option>',
            ...(fonts.uploaded_fonts || []).map(name => `<option value="${ctx.html(name)}"${name === fonts.selected ? ' selected' : ''}>${ctx.html(name)}</option>`)
        ].join('');
        ctx.$('#advanced-oled-font').innerHTML = (fonts.builtin_fonts || []).map(font =>
            `<option value="${font.id}"${font.id === fonts.builtin ? ' selected' : ''}>${ctx.html(font.name)}</option>`
        ).join('');
        ctx.setValue('#oled-font-file', fonts.selected || '');
        ctx.setValue('#oled-font', fonts.builtin ?? 0);
        ctx.$('#delete-fonts').innerHTML = fonts.uploaded_fonts?.length
            ? fonts.uploaded_fonts.map(name => `
                <div class="mini-card font-delete-row">
                    <div><div class="font-name">${ctx.html(name)}</div>${sample(name)}</div>
                    <button class="btn danger small-btn" type="button" data-delete-font="${ctx.html(name)}">Delete</button>
                </div>`).join('')
            : '<p class="small">No uploaded fonts yet.</p>';
        updatePreview();
    };

    const refresh = async () => {
        const [statusResult, fontResult] = await Promise.allSettled([ctx.status.refresh(), loadFonts()]);
        if (statusResult.status === 'fulfilled') applyStatus(statusResult.value || ctx.status.get());
        if (fontResult.status === 'rejected') ctx.setText('#selected-font', 'Could not load fonts');
    };

    ctx.on('click', '[data-clock-action]', async (_, button) => {
        try { await ctx.clock(button.dataset.clockAction, {}, button); } catch (_) {}
    });
    ctx.on('input', '#bedtime-dim-percent', (_, slider) => {
        const percent = setBrightnessValue(slider.value);
        queueBrightnessPreview(percent);
    });
    ctx.on('change', '#bedtime-dim-percent', async (_, slider) => {
        const percent = setBrightnessValue(slider.value);
        window.clearTimeout(brightnessTimer);
        await sendBrightnessPreview(percent);
        try {
            await ctx.update('/api/v1/config/display', {
                method: 'POST',
                body: new URLSearchParams({bedtime_dim_percent: percent, format: 'json'}),
                busyText: `Saving bedtime brightness ${percent}%...`,
                done: `Bedtime brightness saved at ${percent}%`,
                errorText: 'Bedtime brightness could not be saved',
                refreshStatus: false
            });
        } catch (_) {}
    });
    ctx.on('change', '#oled-color', (_, select) => previewOledColour(select.value));
    ctx.on('change', '#advanced-oled-font-file, #advanced-oled-font', (_, field) => {
        ctx.setValue('#oled-font-file', ctx.$('#advanced-oled-font-file').value);
        ctx.setValue('#oled-font', ctx.$('#advanced-oled-font').value);
        updatePreview();
    });
    ctx.on('input', '#advanced-oled-font-size', (_, input) => ctx.setValue('#oled-font-size', input.value));
    ctx.on('click', '[data-delete-font]', async (_, button) => {
        const name = button.dataset.deleteFont;
        if (!confirm(`Delete ${name}?`)) return;
        try {
            await ctx.update('/api/v1/assets/fonts/delete', {
                method: 'POST',
                body: new URLSearchParams({font: name, format: 'json'}),
                button,
                busyText: `Deleting ${name}...`,
                done: `Font deleted: ${name}`,
                errorText: `${name} could not be deleted`
            });
            await refresh();
        } catch (_) {}
    });

    ctx.signal.addEventListener('abort', () => {
        window.clearTimeout(brightnessTimer);
        previewFonts.forEach(font => document.fonts.delete(font));
        previewFonts.clear();
    }, {once: true});

    await refresh();
    return {refresh};
}
