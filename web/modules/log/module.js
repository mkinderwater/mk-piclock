export async function mount(ctx) {
    const render = entries => {
        const list = ctx.$('#log-list');
        list.innerHTML = entries.length
            ? entries.map(line => `<div class="log-line mono">${ctx.html(line)}</div>`).join('')
            : '<div class="log-empty">No log entries yet.</div>';
        list.scrollTop = list.scrollHeight;
    };

    const refresh = async (button, announce = false) => {
        ctx.busy(button, true, 'Refreshing...');
        try {
            const data = await ctx.json('/api/v1/logs');
            const entries = data.entries || [];
            render(entries);
            ctx.setText('#log-meta', `${entries.length} recent entries from ${data.log_file || 'event log'}`);
            if (announce) ctx.notice('Activity refreshed', 'ok', 1600);
        } catch (error) {
            ctx.setText('#log-meta', 'Could not load log.');
            if (announce) ctx.notice(error.message || 'Event log could not be refreshed', 'warn', 3000);
            render([]);
        } finally {
            ctx.busy(button, false);
        }
    };

    ctx.on('click', '#refresh-log', (_, button) => refresh(button, true));
    ctx.on('click', '#clear-log', async (_, button) => {
        if (!confirm('Clear recent activity?')) return;
        try {
            await ctx.update('/api/v1/logs/clear', {
                method: 'POST',
                body: new URLSearchParams({format: 'json'}),
                button,
                busyText: 'Clearing activity...',
                done: 'Activity cleared',
                errorText: 'Activity could not be cleared',
                refreshStatus: false
            });
            await refresh();
        } catch (_) {}
    });

    await refresh();
    return {refresh};
}
