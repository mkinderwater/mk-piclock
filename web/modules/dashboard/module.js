export async function mount(ctx) {
    const preview = ctx.createOledPreview(ctx.$('#oled-screen-preview'));
    let previewLoading = false;
    let previewDelayMs = 250;
    let previewTimer = 0;

    const setPreviewState = (text, type = '') => {
        const node = ctx.$('#oled-preview-state');
        if (!node) return;
        node.textContent = text;
        node.className = `badge ${type}`.trim();
    };

    const refreshPreview = async () => {
        if (previewLoading || ctx.signal.aborted) return;
        previewLoading = true;
        try {
            preview.draw(await ctx.binary('/api/v1/display/preview', {signal: ctx.signal}));
            setPreviewState('Live', 'ok');
        } catch (error) {
            if (!ctx.signal.aborted) setPreviewState('Unavailable', 'warn');
        } finally {
            previewLoading = false;
        }
    };

    const formatAlarmTime = value => {
        const epoch = Number(value || 0);
        if (!epoch) return 'Never';
        return new Date(epoch * 1000).toLocaleString([], {dateStyle: 'medium', timeStyle: 'short'});
    };

    const renderDiagnostics = diagnostics => {
        const warning = ctx.$('#time-sync-warning');
        if (!warning) return;
        warning.classList.toggle('hidden', Boolean(diagnostics?.ntp_synchronized && diagnostics?.system_time_valid));
    };

    const refreshDiagnostics = async () => {
        try { renderDiagnostics(await ctx.json('/api/v1/diagnostics', {signal: ctx.signal})); }
        catch (_) { renderDiagnostics({ntp_synchronized: 0, system_time_valid: 0}); }
    };

    const render = status => {
        if (!status) return;
        preview.setColour(status.oled_color);
        ctx.setText('#status-time', status.time);
        ctx.setText('#status-date', status.date);
        ctx.setText('#status-name', status.clock_name);
        ctx.setText('#status-version', status.app_version);
        ctx.setText('#status-uptime', ctx.formatUptime(status.uptime_seconds));
        const track = [status.audio_title, status.audio_artist].filter(Boolean).join(' - ') || status.audio_file;
        const storyIntro = Boolean(status.story_intro_active);
        const storyText = String(status.story_message || 'STORY MODE!').trim() || 'STORY MODE!';
        ctx.setText('#status-audio', status.alarm_active
            ? `Alarm playing at ${status.alarm_volume_percent || 0}%`
            : (storyIntro ? `Story Mode: ${storyText}` : (status.audio_playing ? `Playing ${track || 'music'}` : 'Audio stopped')));
        ctx.setText('#status-volume', status.alarm_active
            ? `Alarm ${status.alarm_volume_percent || 0}%`
            : `${status.global_volume || 0}%`);
        ctx.setText('#status-display', status.display_mode);
        ctx.setText('#status-oled-color', `${String(status.oled_color || 'green').replace(/^./, value => value.toUpperCase())} panel`);
        ctx.setText('#status-touch', status.touch_ok
            ? (status.touch_pressed ? `Pressed on GPIO ${status.touch_gpio}` : `Ready on GPIO ${status.touch_gpio}`)
            : `Unavailable on GPIO ${status.touch_gpio ?? 20}`);
        ctx.setText('#status-font', status.oled_font_name);
        ctx.setText('#status-bedtime', status.bedtime_enabled
            ? `${ctx.timeValue(status.bedtime_start_hour, status.bedtime_start_min)} to ${ctx.timeValue(status.bedtime_end_hour, status.bedtime_end_min)}, ${status.bedtime_dim_percent}%, ${status.clock_24h_mode ? '24-hour' : '12-hour'}`
            : 'Off');
        ctx.setText('#status-bedtime-music', status.bedtime_music_enabled ? 'Allowed' : 'Disabled');
        ctx.setText('#status-next-alarm', status.next_alarm_text || 'No alarm scheduled');
        ctx.setText('#status-last-alarm', formatAlarmTime(status.last_successful_alarm));
        ctx.setText('#summary-clock', status.oled_ok ? `Working · ${status.time || ''}` : 'Screen unavailable');
        ctx.setText('#summary-sound', status.alarm_active
            ? `Alarm playing · ${status.alarm_volume_percent || 0}%`
            : (storyIntro ? `Story Mode · ${storyText}` : (status.audio_playing ? `Playing ${track || 'music'}` : 'None')));
        ctx.setText('#summary-bedtime', status.bedtime_enabled
            ? `${ctx.timeValue(status.bedtime_start_hour, status.bedtime_start_min)} to ${ctx.timeValue(status.bedtime_end_hour, status.bedtime_end_min)}`
            : 'Not scheduled');
        ctx.setText('#summary-next-alarm', status.next_alarm_text || 'None scheduled');
        previewDelayMs = storyIntro || (status.audio_playing && status.show_song_metadata) ? 100 : 250;
    };

    ctx.status.subscribe(render);
    ctx.on('click', '[data-clock-action]', async (_, button) => {
        try {
            await ctx.clock(button.dataset.clockAction, {}, button);
            await refreshPreview();
        } catch (_) {}
    });
    ctx.on('click', '#play-first-song', async (_, button) => {
        let music;
        try {
            music = await ctx.json('/api/v1/assets/music');
        } catch (error) {
            ctx.notice(error.message || 'Music library could not be loaded', 'warn', 3000);
            return;
        }

        const track = music.tracks?.[0];
        const file = track?.file || '';
        if (!file) {
            ctx.notice('Add music on the Music page first.', 'warn', 3000);
            return;
        }

        const label = track?.display || track?.title || file;
        try {
            await ctx.clock('play-music', {file}, button, {
                busyText: `Starting ${label}...`,
                done: `Playing ${label}`,
                errorText: `${label} could not be played`
            });
            await refreshPreview();
        } catch (_) {}
    });

    await Promise.allSettled([ctx.status.refresh(), refreshPreview(), refreshDiagnostics()]);

    const statusTimer = window.setInterval(() => {
        ctx.status.refresh().catch(() => {});
    }, 5000);
    const diagnosticsTimer = window.setInterval(() => {
        refreshDiagnostics().catch(() => {});
    }, 15000);
    const schedulePreview = () => {
        if (ctx.signal.aborted) return;
        previewTimer = window.setTimeout(async () => {
            await refreshPreview();
            schedulePreview();
        }, previewDelayMs);
    };
    schedulePreview();
    ctx.signal.addEventListener('abort', () => {
        window.clearInterval(statusTimer);
        window.clearInterval(diagnosticsTimer);
        window.clearTimeout(previewTimer);
    }, {once: true});
}
