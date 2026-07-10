const scenes = Object.freeze([
    {id: 'alarm', name: 'Alarm', description: 'Used while an alarm is ringing.'},
    {id: 'bedtime', name: 'Bedtime', description: 'Used during the bedtime schedule.'},
    {id: 'message', name: 'Message', description: 'Used while a sent message is shown.'},
    {id: 'music', name: 'Music', description: 'Used while normal music is playing.'},
    {id: 'daytime', name: 'Daytime', description: 'Used when no higher-priority activity is active.'},
    {id: 'stories', name: 'Stories', description: 'Used while Story Mode audio is playing.'}
]);

const profileDefaults = Object.freeze({
    alarm: {effect: 'fade', brightness: 70, cycle_seconds: 8, color1: '#ff0000', color2: '#ffa000'},
    bedtime: {effect: 'solid', brightness: 3, cycle_seconds: 8, color1: '#ff3000', color2: '#ff3000'},
    message: {effect: 'fade', brightness: 35, cycle_seconds: 8, color1: '#0096ff', color2: '#ffffff'},
    music: {effect: 'rainbow', brightness: 40, cycle_seconds: 12, color1: '#ff0000', color2: '#0000ff'},
    daytime: {effect: 'solid', brightness: 5, cycle_seconds: 8, color1: '#ffdca0', color2: '#ffdca0'},
    stories: {effect: 'fade', brightness: 15, cycle_seconds: 8, color1: '#7d2dff', color2: '#005aff'}
});

const globalDefaults = Object.freeze({
    enabled: true,
    max_brightness: 100,
    red_gain: 100,
    green_gain: 65,
    blue_gain: 80,
    idle_off: false,
    bedtime_fade_minutes: 15,
    touch_blink_enabled: true,
    touch_blink_brightness: 60,
    touch_blink_color: '#ffffff'
});

const effectLabel = effect => ({solid: 'Solid colour', fade: 'Colour fade', rainbow: 'Rainbow'}[effect] || 'Solid colour');

function cardTemplate(scene) {
    return `
        <form class="card lighting-card" data-scene="${scene.id}" data-module-handles-submit="true">
            <div class="card-title-row">
                <h2>${scene.name}</h2>
                <span class="badge lighting-effect-badge">Solid colour</span>
            </div>
            <p class="small no-margin">${scene.description}</p>
            <div class="lighting-summary">
                <span class="lighting-swatch" aria-hidden="true"></span>
                <span class="lighting-summary-text">Loading</span>
            </div>
            <input type="hidden" name="scene" value="${scene.id}">
            <div class="lighting-fields">
                <div>
                    <label for="${scene.id}-effect">Effect</label>
                    <select id="${scene.id}-effect" name="effect">
                        <option value="solid">Solid colour</option>
                        <option value="fade">Fade between two colours</option>
                        <option value="rainbow">Rainbow</option>
                    </select>
                </div>
                <div class="lighting-colours">
                    <div class="colour-one-field">
                        <label for="${scene.id}-color1">Colour</label>
                        <input id="${scene.id}-color1" type="color" name="color1" value="#ffffff">
                    </div>
                    <div class="colour-two-field">
                        <label for="${scene.id}-color2">Fade to</label>
                        <input id="${scene.id}-color2" type="color" name="color2" value="#0000ff">
                    </div>
                </div>
                <div>
                    <label for="${scene.id}-brightness">Brightness <output for="${scene.id}-brightness">50%</output></label>
                    <input id="${scene.id}-brightness" type="range" name="brightness" min="0" max="100" step="1" value="50">
                    <p class="small lighting-effect-note">Set brightness to 0% to keep this mode dark.</p>
                </div>
                <div class="lighting-speed-field">
                    <label for="${scene.id}-cycle">Effect cycle <output for="${scene.id}-cycle">8 sec</output></label>
                    <input id="${scene.id}-cycle" type="range" name="cycle_seconds" min="2" max="60" step="1" value="8">
                </div>
            </div>
            <div class="lighting-actions lighting-profile-actions">
                <button class="btn alt" type="button" data-profile-reset>Reset</button>
                <button class="btn alt" type="button" data-led-preview>Preview 10 Seconds</button>
                <button class="btn" type="submit">Save ${scene.name}</button>
            </div>
        </form>`;
}

export async function mount(ctx) {
    const host = ctx.$('#lighting-profiles');
    const globalForm = ctx.$('#lighting-global-form');
    host.innerHTML = scenes.map(cardTemplate).join('');

    const profileBody = form => {
        const body = new URLSearchParams(new FormData(form));
        body.set('format', 'json');
        return body;
    };

    const globalBody = () => {
        const body = new URLSearchParams(new FormData(globalForm));
        body.set('enabled', globalForm.elements.enabled.checked ? '1' : '0');
        body.set('idle_off', globalForm.elements.idle_off.checked ? '1' : '0');
        body.set('touch_blink_enabled', globalForm.elements.touch_blink_enabled.checked ? '1' : '0');
        body.set('format', 'json');
        return body;
    };

    const setProfileValues = (form, values) => {
        form.elements.effect.value = values.effect;
        form.elements.brightness.value = values.brightness;
        form.elements.cycle_seconds.value = values.cycle_seconds;
        form.elements.color1.value = values.color1;
        form.elements.color2.value = values.color2;
    };

    const updateCard = form => {
        const effect = form.elements.effect.value;
        const brightness = Number.parseInt(form.elements.brightness.value, 10) || 0;
        const cycleSeconds = Number.parseInt(form.elements.cycle_seconds.value, 10) || 8;
        form.querySelector('output[for$="-brightness"]').textContent = `${brightness}%`;
        form.querySelector('output[for$="-cycle"]').textContent = `${cycleSeconds} sec`;
        form.querySelector('.colour-one-field').classList.toggle('hidden', effect === 'rainbow');
        form.querySelector('.colour-two-field').classList.toggle('hidden', effect !== 'fade');
        form.querySelector('.lighting-speed-field').classList.toggle('hidden', effect === 'solid');
        form.querySelector('.lighting-effect-badge').textContent = effectLabel(effect);
        const swatch = form.querySelector('.lighting-swatch');
        const color1 = form.elements.color1.value;
        const color2 = form.elements.color2.value;
        swatch.style.background = effect === 'rainbow'
            ? 'conic-gradient(red, yellow, lime, cyan, blue, magenta, red)'
            : effect === 'fade'
                ? `linear-gradient(135deg, ${color1}, ${color2})`
                : color1;
        const timing = effect === 'solid' ? '' : `, ${cycleSeconds} sec cycle`;
        form.querySelector('.lighting-summary-text').textContent = `${effectLabel(effect)}, ${brightness}%${timing}`;
    };

    const setGlobalValues = values => {
        globalForm.elements.enabled.checked = Boolean(values.enabled);
        globalForm.elements.max_brightness.value = values.max_brightness;
        globalForm.elements.red_gain.value = values.red_gain;
        globalForm.elements.green_gain.value = values.green_gain;
        globalForm.elements.blue_gain.value = values.blue_gain;
        globalForm.elements.idle_off.checked = Boolean(values.idle_off);
        globalForm.elements.bedtime_fade_minutes.value = values.bedtime_fade_minutes;
        globalForm.elements.touch_blink_enabled.checked = values.touch_blink_enabled !== false && values.touch_blink_enabled !== 0;
        globalForm.elements.touch_blink_brightness.value = values.touch_blink_brightness ?? 60;
        globalForm.elements.touch_blink_color.value = values.touch_blink_color || '#ffffff';
        updateGlobalOutputs();
    };

    const updateGlobalOutputs = () => {
        for (const input of globalForm.querySelectorAll('input[type="range"]')) {
            const output = globalForm.querySelector(`output[for="${input.id}"]`);
            if (!output) continue;
            output.textContent = input.name === 'bedtime_fade_minutes'
                ? `${input.value} min`
                : `${input.value}%`;
        }
    };

    const markProfileDirty = form => {
        form.dataset.dirty = 'true';
        form.dataset.revision = String((Number.parseInt(form.dataset.revision || '0', 10) || 0) + 1);
    };

    const applyStatus = status => {
        if (!status) return;
        const ready = Boolean(status.led_ok);
        const badge = ctx.$('#lighting-status');
        badge.textContent = ready ? 'Lighting ready' : 'LED GPIO unavailable';
        badge.className = `badge ${ready ? 'ok' : 'warn'}`;
        ctx.setText('#lighting-active-mode', String(status.led_scene || 'daytime').replace(/^./, value => value.toUpperCase()));
        ctx.setText('#lighting-live-colour', status.led_colour || '#000000');
        ctx.setText('#lighting-live-output', status.led_output || '#000000');
        ctx.$('#lighting-live-colour-swatch').style.background = status.led_colour || '#000000';
        ctx.$('#lighting-live-output-swatch').style.background = status.led_output || '#000000';
        const fadeText = status.led_bedtime_fade_active ? ' Bedtime fade active.' : '';
        ctx.setText('#lighting-runtime-details', `Priority: touch, alarm, message, stories, music, bedtime, daytime. ${status.led_pwm_hz || 200} Hz PWM, ${status.led_pwm_levels || 32} levels. GPIO errors: ${status.led_write_errors || 0}.${fadeText}`);

        for (const button of [...host.querySelectorAll('[data-led-preview]'), ...globalForm.querySelectorAll('[data-led-test]')]) {
            button.disabled = !ready;
            button.title = ready ? '' : 'LED GPIO unavailable';
        }

        if (globalForm.dataset.dirty !== 'true') setGlobalValues(status.led_settings || globalDefaults);
        for (const profile of status.led_profiles || []) {
            const form = host.querySelector(`[data-scene="${profile.scene}"]`);
            if (!form || form.dataset.dirty === 'true') continue;
            setProfileValues(form, {
                effect: profile.effect || 'solid',
                brightness: profile.brightness ?? 0,
                cycle_seconds: profile.cycle_seconds ?? 8,
                color1: profile.color1 || '#ffffff',
                color2: profile.color2 || '#0000ff'
            });
            updateCard(form);
        }
    };

    ctx.on('input', '.lighting-card input, .lighting-card select', (_, field) => {
        markProfileDirty(field.form);
        updateCard(field.form);
    });
    const markGlobalDirty = () => {
        globalForm.dataset.dirty = 'true';
        globalForm.dataset.revision = String((Number.parseInt(globalForm.dataset.revision || '0', 10) || 0) + 1);
    };

    ctx.on('input', '#lighting-global-form input', () => {
        markGlobalDirty();
        updateGlobalOutputs();
    });
    ctx.on('change', '#lighting-global-form input[type="checkbox"]', markGlobalDirty);

    ctx.on('click', '[data-profile-reset]', (_, button) => {
        const form = button.closest('form');
        setProfileValues(form, profileDefaults[form.dataset.scene]);
        markProfileDirty(form);
        updateCard(form);
    });
    ctx.on('click', '[data-global-reset]', () => {
        setGlobalValues(globalDefaults);
        markGlobalDirty();
    });

    ctx.on('submit', '.lighting-card', async (event, form) => {
        event.preventDefault();
        const button = event.submitter || form.querySelector('[type="submit"]');
        const revision = form.dataset.revision || '0';
        try {
            await ctx.update('/api/v1/config/led', {
                method: 'POST',
                body: profileBody(form),
                button,
                busyText: 'Saving lighting profile...',
                done: 'Lighting profile saved',
                errorText: 'Lighting profile could not be saved',
                refreshStatus: false
            });
            if ((form.dataset.revision || '0') === revision) {
                form.dataset.dirty = 'false';
                await refresh();
            }
        } catch (_) {}
    });

    ctx.on('submit', '#lighting-global-form', async (event, form) => {
        event.preventDefault();
        const button = event.submitter || form.querySelector('[type="submit"]');
        const revision = form.dataset.revision || '0';
        try {
            await ctx.update('/api/v1/config/led-global', {
                method: 'POST',
                body: globalBody(),
                button,
                busyText: 'Saving global lighting controls...',
                done: 'Global lighting controls saved',
                errorText: 'Global lighting controls could not be saved',
                refreshStatus: false
            });
            if ((form.dataset.revision || '0') === revision) {
                form.dataset.dirty = 'false';
                await refresh();
            }
        } catch (_) {}
    });

    ctx.on('click', '[data-led-preview]', async (_, button) => {
        const form = button.closest('form');
        const body = profileBody(form);
        body.set('hold_seconds', '10');
        body.set('bypass_master', '1');
        try {
            await ctx.update('/api/v1/led/preview', {
                method: 'POST',
                body,
                button,
                busyText: 'Starting LED preview...',
                done: 'LED preview started',
                errorText: 'LED preview could not start',
                refreshStatus: false
            });
        } catch (_) {}
    });

    ctx.on('click', '[data-led-test]', async (_, button) => {
        const colour = button.dataset.ledTest;
        const body = new URLSearchParams({
            scene: 'daytime', effect: 'solid', brightness: '50', cycle_seconds: '8',
            color1: colour, color2: colour, hold_seconds: '3', bypass_master: '1', raw_output: '1', format: 'json'
        });
        try {
            await ctx.update('/api/v1/led/preview', {
                method: 'POST', body, button,
                busyText: `Testing ${button.textContent.replace('Test ', '').toLowerCase()} channel...`,
                done: `${button.textContent} started`,
                errorText: 'LED test could not start',
                refreshStatus: false
            });
        } catch (_) {}
    });

    const refresh = async () => applyStatus(await ctx.status.refresh());
    ctx.status.subscribe(applyStatus);
    applyStatus(ctx.status.get());
    const statusTimer = window.setInterval(() => refresh().catch(() => {}), 3000);
    ctx.signal.addEventListener('abort', () => window.clearInterval(statusTimer), {once: true});
    return {refresh};
}
