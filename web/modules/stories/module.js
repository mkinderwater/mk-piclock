import {audioTrackFacts} from '/assets/js/audio-library.js?v=1.7.6';

export async function mount(ctx) {
    const renderStatus = status => {
        if (!status) return;
        const enabled = Boolean(status.story_mode_enabled);
        ctx.setText('#story-mode-status', enabled ? 'Enabled' : 'Disabled');
        const introText = String(status.story_message || 'STORY MODE!').trim() || 'STORY MODE!';
        const current = status.story_intro_active
            ? `Intro: ${introText}`
            : (status.story_playing
                ? ([status.audio_title, status.audio_artist].filter(Boolean).join(' - ') || status.audio_file || 'Playing')
                : 'Quiet');
        ctx.setText('#story-current', current);
        const toggle = ctx.$('#story-enabled');
        if (toggle) toggle.checked = enabled;
        ctx.setValue('#story-enabled-value', enabled ? 1 : 0);
        ctx.setValue('#story-volume', status.story_volume ?? 55);
        ctx.setText('#story-volume-value', `${status.story_volume ?? 55}%`);
        ctx.setValue('#story-message', status.story_message || 'STORY MODE!');
    };

    const refreshLibrary = async () => {
        try {
            const data = await ctx.json('/api/v1/assets/stories');
            const tracks = data.tracks || [];
            ctx.setText('#story-count', `${tracks.length} stor${tracks.length === 1 ? 'y' : 'ies'}`);
            ctx.$('#story-list').innerHTML = tracks.length
                ? tracks.map(track => {
                    const facts = audioTrackFacts(track, {channels: false, layer: false});
                    const label = track.display || track.title || track.file;
                    return `
                    <div class="mini-card story-card">
                        <div class="story-details">
                            <div class="font-name">${ctx.html(track.title || track.file)}</div>
                            ${track.artist ? `<div class="story-artist">${ctx.html(track.artist)}</div>` : ''}
                            ${facts.length ? `<div class="story-facts">${facts.map(value => `<span>${ctx.html(value)}</span>`).join('')}</div>` : ''}
                            <div class="small muted story-file">${ctx.html(track.file)}${track.id3 ? ' · ID3 tags' : ''}</div>
                        </div>
                        <div class="mini-actions">
                            <button class="btn ok small-btn" type="button" data-story-play="${ctx.html(track.file)}" data-story-label="${ctx.html(label)}">Play</button>
                            <button class="btn small-btn" type="button" data-story-stop>Stop</button>
                            <button class="btn danger small-btn" type="button" data-story-delete="${ctx.html(track.file)}" data-story-label="${ctx.html(label)}">Delete</button>
                        </div>
                    </div>`;
                }).join('')
                : '<p class="small">No story MP3 files yet.</p>';
        } catch (_) {
            ctx.$('#story-list').innerHTML = '<p class="small">Could not load stories.</p>';
        }
    };

    ctx.on('change', '#story-enabled', (_, checkbox) => {
        ctx.setValue('#story-enabled-value', checkbox.checked ? 1 : 0);
    });
    ctx.on('input', '#story-volume', (_, slider) => {
        ctx.setText('#story-volume-value', `${slider.value}%`);
    });

    ctx.on('submit', '#story-upload-form', async (event, form) => {
        event.preventDefault();
        const input = form.querySelector('#story-file');
        if (input?.files?.length !== 1) {
            ctx.notice('Select one MP3 file.', 'warn', 3000);
            return;
        }
        const button = event.submitter || form.querySelector('[type="submit"]');
        try {
            await ctx.update(form.action, {
                method: 'POST', body: new FormData(form), button,
                busyText: 'Uploading story...', done: 'Story added', errorText: 'Story could not be added',
                refreshStatus: false
            });
            form.reset();
            await refreshLibrary();
        } catch (_) {}
    });

    const storyIntroNotice = () => {
        const fromInput = ctx.$('#story-message')?.value;
        const fromStatus = ctx.status.get()?.story_message;
        const text = String(fromInput || fromStatus || 'STORY MODE!').trim() || 'STORY MODE!';
        return `Story Mode: ${text}`;
    };

    ctx.on('click', '#play-random-story', async (_, button) => {
        try {
            await ctx.clock('play-story', {}, button, {
                busyText: 'Choosing a story...', done: storyIntroNotice(), doneTimeout: 3000,
                errorText: 'A story could not be started'
            });
            renderStatus(ctx.status.get());
        } catch (_) {}
    });
    ctx.on('click', '[data-story-play]', async (_, button) => {
        const label = button.dataset.storyLabel || button.dataset.storyPlay;
        try {
            await ctx.clock('play-story', {file: button.dataset.storyPlay}, button, {
                busyText: `Starting ${label}...`, done: storyIntroNotice(), doneTimeout: 3000,
                errorText: `${label} could not be played`
            });
            renderStatus(ctx.status.get());
        } catch (_) {}
    });
    ctx.on('click', '[data-story-stop]', async (_, button) => {
        try {
            await ctx.clock('stop', {}, button);
            renderStatus(ctx.status.get());
        } catch (_) {}
    });
    ctx.on('click', '[data-story-delete]', async (_, button) => {
        const file = button.dataset.storyDelete;
        const label = button.dataset.storyLabel || file;
        if (!confirm(`Delete ${label}?`)) return;
        try {
            await ctx.update('/api/v1/assets/stories/delete', {
                method: 'POST', body: new URLSearchParams({file, format: 'json'}), button,
                busyText: `Deleting ${label}...`, done: `${label} deleted`, errorText: `${label} could not be deleted`
            });
            await refreshLibrary();
        } catch (_) {}
    });
    ctx.on('click', '#delete-all-stories', async (_, button) => {
        if (!confirm('Delete ALL story MP3 files?')) return;
        try {
            await ctx.update('/api/v1/assets/stories/delete-all', {
                method: 'POST', body: new URLSearchParams({format: 'json'}), button,
                busyText: 'Deleting all stories...', done: 'All stories deleted', errorText: 'Story library could not be cleared'
            });
            await refreshLibrary();
        } catch (_) {}
    });

    const refresh = async () => {
        const [statusResult] = await Promise.allSettled([ctx.status.refresh(), refreshLibrary()]);
        if (statusResult.status === 'fulfilled') renderStatus(statusResult.value || ctx.status.get());
    };

    await refresh();
    return {refresh};
}
