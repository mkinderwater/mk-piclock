import {audioTrackFacts, formatAudioBytes} from '/assets/js/audio-library.js?v=1.8.1';

export async function mount(ctx) {
    const renderStatus = status => {
        if (!status) return;
        ctx.setText('#music-status', status.audio_playing ? 'Playing' : 'None');
        const metadata = [status.audio_title, status.audio_artist].filter(Boolean).join(' - ');
        ctx.setText('#music-current', metadata || status.audio_file || '-');
        ctx.setValue('#global-volume', status.global_volume ?? 80);
        ctx.setText('#global-volume-value', `${status.global_volume ?? 80}%`);
        const metadataToggle = ctx.$('#show-song-metadata');
        if (metadataToggle) metadataToggle.checked = !!status.show_song_metadata;
        ctx.setValue('#show-song-metadata-value', status.show_song_metadata ? 1 : 0);
    };

    const refreshLibrary = async () => {
        try {
            const data = await ctx.json('/api/v1/assets/music');
            const tracks = data.tracks || [];
            ctx.setText('#music-count', `${tracks.length} song${tracks.length === 1 ? '' : 's'}`);
            ctx.setValue('#global-volume', data.global_volume ?? 80);
            ctx.setText('#global-volume-value', `${data.global_volume ?? 80}%`);
            ctx.$('#music-list').innerHTML = tracks.length
                ? tracks.map(track => {
                    const facts = audioTrackFacts(track);
                    const details = [
                        track.album ? `Album: ${track.album}` : '',
                        track.year ? `Year: ${track.year}` : '',
                        track.track ? `Track: ${track.track}` : '',
                        track.genre ? `Genre: ${track.genre}` : ''
                    ].filter(Boolean);
                    return `
                    <div class="mini-card song-card">
                        <div class="song-details">
                            <div class="font-name">${ctx.html(track.title || track.file)}</div>
                            ${track.artist ? `<div class="song-artist">${ctx.html(track.artist)}</div>` : ''}
                            ${(details.length || facts.length) ? `<details class="file-details"><summary>Song details</summary>
                                ${details.length ? `<div class="song-tags">${details.map(value => `<span>${ctx.html(value)}</span>`).join('')}</div>` : ''}
                                ${facts.length ? `<div class="song-facts">${facts.map(value => `<span>${ctx.html(value)}</span>`).join('')}</div>` : ''}
                                <div class="small muted song-file">${ctx.html(track.file)}${track.id3 ? ' · ID3 tags' : ''}</div>
                            </details>` : `<div class="small muted song-file">${ctx.html(track.file)}</div>`}
                        </div>
                        <div class="mini-actions">
                            <button class="btn ok small-btn" type="button" data-music-play="${ctx.html(track.file)}" data-music-label="${ctx.html(track.display || track.title || track.file)}">Play</button>
                            <button class="btn small-btn" type="button" data-music-stop>Stop</button>
                            <button class="btn danger small-btn" type="button" data-music-delete="${ctx.html(track.file)}" data-music-label="${ctx.html(track.display || track.title || track.file)}">Delete</button>
                        </div>
                    </div>`;
                }).join('')
                : '<p class="small">No processed MP3 files yet.</p>';
        } catch (_) {
            ctx.$('#music-list').innerHTML = '<p class="small">Could not load music.</p>';
        }
    };

    let knownJobStates = new Map();
    let jobTimer = 0;
    const selectedFiles = () => Array.from(ctx.$('#music-files')?.files || []);
    const updateSelectionSummary = () => {
        const files = selectedFiles();
        const summary = ctx.$('#music-selection-summary');
        if (!summary) return;
        if (!files.length) {
            summary.textContent = 'No songs selected.';
            return;
        }
        const totalBytes = files.reduce((sum, file) => sum + (Number(file.size) || 0), 0);
        const total = formatAudioBytes(totalBytes);
        summary.textContent = `${files.length} song${files.length === 1 ? '' : 's'} selected${total ? ` · ${total}` : ''}.`;
    };
    const scheduleJobRefresh = delay => {
        window.clearTimeout(jobTimer);
        if (!ctx.signal.aborted) jobTimer = window.setTimeout(refreshJobs, delay);
    };
    const refreshJobs = async () => {
        try {
            const data = await ctx.json('/api/v1/assets/music/jobs');
            const jobs = data.jobs || [];
            let libraryChanged = false;
            for (const job of jobs) {
                const previous = knownJobStates.get(job.id);
                if (previous && previous !== job.state && (job.state === 'complete' || job.state === 'failed'))
                    libraryChanged = true;
                knownJobStates.set(job.id, job.state);
            }
            if (libraryChanged) await refreshLibrary();

            const shown = [
                ...jobs.filter(job => job.state === 'processing'),
                ...jobs.filter(job => job.state === 'queued'),
                ...jobs.filter(job => job.state !== 'processing' && job.state !== 'queued')
            ].slice(0, 10);
            ctx.$('#music-processing-card').classList.toggle('hidden', shown.length === 0);
            ctx.$('#music-jobs').innerHTML = shown.length
                ? shown.map(job => {
                    const active = job.state === 'queued' || job.state === 'processing';
                    const label = job.state === 'queued' ? 'Queued' :
                        job.state === 'processing' ? 'Processing' :
                        job.state === 'complete' ? 'Ready' : 'Failed';
                    const note = job.error
                        ? `<div class="small ${job.state === 'failed' ? 'job-error' : 'muted'}">${ctx.html(job.error)}</div>`
                        : '';
                    return `
                    <div class="mini-card music-job ${ctx.html(job.state)}">
                        <div class="job-head">
                            <strong>${ctx.html(job.file)}</strong>
                            <span class="job-state">${label}</span>
                        </div>
                        <div class="small muted">Mono · ${job.bitrate_kbps} kbps · ${(job.sample_rate_hz / 1000).toFixed(job.sample_rate_hz % 1000 ? 1 : 0)} kHz · ${Math.round(job.lowpass_hz / 1000)} kHz limit</div>
                        ${active ? `<progress max="100" value="${Number(job.progress) || 0}"></progress><div class="small muted">${Number(job.progress) || 0}%</div>` : ''}
                        ${note}
                    </div>`;
                }).join('')
                : '';

            const active = jobs.some(job => job.state === 'queued' || job.state === 'processing');
            const queued = jobs.some(job => job.state === 'queued');
            const fileInput = ctx.$('#music-files');
            const uploadButton = ctx.$('#music-upload-form [type="submit"]');
            const clearButton = ctx.$('#clear-music-queue');
            if (fileInput) fileInput.disabled = active;
            if (uploadButton) {
                uploadButton.disabled = active;
                uploadButton.textContent = active ? 'Preparing Music...' : 'Add Music';
            }
            if (clearButton) clearButton.disabled = !queued;
            scheduleJobRefresh(active ? 1000 : 8000);
        } catch (_) {
            ctx.$('#music-processing-card').classList.remove('hidden');
            ctx.$('#music-jobs').innerHTML = '<p class="small">Music preparation status could not be loaded.</p>';
            scheduleJobRefresh(8000);
        }
    };

    ctx.on('change', '#music-files', () => updateSelectionSummary());

    ctx.on('submit', '#music-upload-form', async (event, form) => {
        event.preventDefault();
        const button = event.submitter || form.querySelector('[type="submit"]');
        const files = form.querySelector('#music-files');
        if (!files?.files?.length) {
            ctx.notice('Select one or more MP3 files.', 'warn', 3000);
            return;
        }
        if (files.files.length > 32) {
            ctx.notice('Select 32 MP3 files or fewer.', 'warn', 3000);
            return;
        }
        try {
            const jobs = await ctx.json('/api/v1/assets/music/jobs');
            if ((jobs.jobs || []).some(job => job.state === 'queued' || job.state === 'processing')) {
                ctx.notice('Wait for all music to finish before uploading more.', 'warn', 4000);
                await refreshJobs();
                return;
            }
            const count = files.files.length;
            const response = await ctx.update(form.action, {
                method: 'POST',
                body: new FormData(form),
                button,
                busyText: 'Uploading music...',
                done: count === 1 ? 'Music is being prepared' : 'Music batch is being prepared',
                errorText: 'Music could not be added',
                refreshStatus: false
            });
            const result = await response.json();
            files.value = '';
            updateSelectionSummary();
            ctx.notice(`${result.queued || count} song${(result.queued || count) === 1 ? '' : 's'} added. Preparing now.`, 'ok', 3000);
            await refreshJobs();
        } catch (_) {}
    });

    ctx.on('click', '#clear-music-queue', async (_, button) => {
        if (!confirm('Remove all waiting MP3 uploads? A song already processing will continue.')) return;
        try {
            const response = await ctx.update('/api/v1/assets/music/jobs/clear', {
                method: 'POST',
                body: new URLSearchParams({format: 'json'}),
                button,
                busyText: 'Clearing queue...',
                done: 'Waiting music cleared',
                errorText: 'Music queue could not be cleared',
                refreshStatus: false
            });
            const result = await response.json();
            if (result.processing_active)
                ctx.notice(`${result.queued_removed} waiting file${result.queued_removed === 1 ? '' : 's'} removed. The current song is still processing.`, 'warn', 5000);
            await refreshJobs();
        } catch (_) {}
    });

    ctx.on('click', '[data-music-play]', async (_, button) => {
        try {
            const label = button.dataset.musicLabel || button.dataset.musicPlay;
            await ctx.clock('play-music', {file: button.dataset.musicPlay}, button, {
                busyText: `Starting ${label}...`,
                done: `Playing ${label}`,
                errorText: `${label} could not be played`
            });
            renderStatus(ctx.status.get());
        } catch (_) {}
    });
    ctx.on('click', '[data-music-stop]', async (_, button) => {
        try {
            await ctx.clock('stop', {}, button);
            renderStatus(ctx.status.get());
        } catch (_) {}
    });
    ctx.on('click', '[data-music-delete]', async (_, button) => {
        const file = button.dataset.musicDelete;
        const label = button.dataset.musicLabel || file;
        if (!confirm(`Delete ${label}? Alarms using it will return to Random.`)) return;
        try {
            await ctx.update('/api/v1/assets/music/delete', {
                method: 'POST',
                body: new URLSearchParams({file, format: 'json'}),
                button,
                busyText: `Deleting ${label}...`,
                done: `${label} deleted`,
                errorText: `${label} could not be deleted`
            });
            await refreshLibrary();
        } catch (_) {}
    });
    ctx.on('click', '#delete-all-music', async (_, button) => {
        if (!confirm('Delete ALL processed MP3 files and reset alarm music choices to Random?')) return;
        try {
            await ctx.update('/api/v1/assets/music/delete-all', {
                method: 'POST',
                body: new URLSearchParams({format: 'json'}),
                button,
                busyText: 'Deleting all music...',
                done: 'All processed music deleted',
                errorText: 'Music library could not be cleared'
            });
            await refreshLibrary();
        } catch (_) {}
    });

    ctx.on('input', '#global-volume', (_, slider) => {
        ctx.setText('#global-volume-value', `${slider.value}%`);
    });

    ctx.on('change', '#show-song-metadata', (_, checkbox) => {
        ctx.setValue('#show-song-metadata-value', checkbox.checked ? 1 : 0);
    });

    const refresh = async () => {
        const [statusResult] = await Promise.allSettled([
            ctx.status.refresh(),
            refreshLibrary(),
            refreshJobs()
        ]);
        if (statusResult.status === 'fulfilled') renderStatus(statusResult.value || ctx.status.get());
    };

    ctx.signal.addEventListener('abort', () => window.clearTimeout(jobTimer), {once: true});
    await refresh();
    return {refresh};
}
