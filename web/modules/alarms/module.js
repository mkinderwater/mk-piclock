const dayNames = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];

export async function mount(ctx) {
    let musicFiles = [];

    const musicOptions = selected => [
        '<option value="">Choose a random song</option>',
        ...musicFiles.map(name => `<option value="${ctx.html(name)}"${name === selected ? ' selected' : ''}>${ctx.html(name)}</option>`)
    ].join('');

    const dayCheckboxes = mask => `<div class="day-grid">${dayNames.map((name, day) =>
        `<label class="day-pill"><input type="checkbox" name="day${day}" value="1"${mask & (1 << day) ? ' checked' : ''}> ${name}</label>`
    ).join('')}</div>`;

    const render = status => {
        const list = ctx.$('#alarm-list');
        const alarms = Array.isArray(status?.alarms) ? status.alarms : [];
        if (!alarms.length) {
            list.innerHTML = '<div class="card"><p>No alarms are configured.</p></div>';
            return;
        }
        list.innerHTML = alarms.map(alarm => `
            <div class="card alarm-card">
                <div class="card-title-row"><h2>Alarm ${alarm.id}</h2><span class="badge ${alarm.enabled ? 'ok' : ''}">${alarm.enabled ? 'On' : 'Off'}</span></div>
                <form method="POST" action="/api/v1/config/alarms" data-busy-text="Saving alarm ${alarm.id}..." data-success-text="Alarm ${alarm.id} saved" data-error-text="Alarm ${alarm.id} could not be saved">
                    <input type="hidden" name="id" value="${alarm.id}">
                    <div class="field-row">
                        <div><label>Alarm</label><select name="enabled"><option value="1"${alarm.enabled ? ' selected' : ''}>On</option><option value="0"${!alarm.enabled ? ' selected' : ''}>Off</option></select></div>
                        <div><label>Time</label><input type="time" name="time" value="${ctx.timeValue(alarm.hour, alarm.min)}"></div>
                    </div>
                    <label>Days</label>${dayCheckboxes(alarm.weekdays)}
                    <label>Wake-up music</label><select name="music_file">${musicOptions(alarm.music_file)}</select>
                    <div class="field-row">
                        <div><label>Starting volume</label><input type="number" name="start_volume" min="0" max="100" value="${alarm.start_volume}"></div>
                        <div><label>Final volume</label><input type="number" name="end_volume" min="0" max="100" value="${alarm.end_volume}"></div>
                    </div>
                    <button class="btn" type="submit">Save Alarm</button>
                </form>
            </div>`).join('');
    };

    const refresh = async () => {
        try {
            const [music, status] = await Promise.all([
                ctx.json('/api/v1/assets/music'),
                ctx.status.refresh()
            ]);
            musicFiles = (music.tracks || []).map(track => track.file);
            render(status || ctx.status.get());
        } catch (_) {
            ctx.$('#alarm-list').innerHTML = '<div class="card"><p>Could not load alarms.</p></div>';
        }
    };

    await refresh();
    return {refresh};
}
