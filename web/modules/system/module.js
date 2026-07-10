export async function mount(ctx) {
    const yesNo = value => value ? 'Yes' : 'No';
    const working = value => value ? 'Working' : 'Unavailable';
    const available = value => value || 'Unavailable';
    const bytes = value => {
        if (value === null || value === undefined || value === '') return 'Unavailable';
        const amount = Number(value);
        if (!Number.isFinite(amount) || amount < 0) return 'Unavailable';
        const units = ['B', 'KB', 'MB', 'GB', 'TB'];
        let size = amount;
        let index = 0;
        while (size >= 1024 && index < units.length - 1) { size /= 1024; index++; }
        return `${size.toFixed(index < 2 ? 0 : 1)} ${units[index]}`;
    };
    const directoryUsage = (size, count) => {
        const files = Math.max(0, Number(count || 0));
        return `${bytes(size)} in ${files.toLocaleString()} file${files === 1 ? '' : 's'}`;
    };
    const uptime = value => {
        let seconds = Math.max(0, Number(value || 0));
        if (!seconds) return 'Unavailable';
        const days = Math.floor(seconds / 86400);
        seconds %= 86400;
        const hours = Math.floor(seconds / 3600);
        seconds %= 3600;
        const minutes = Math.floor(seconds / 60);
        const parts = [];
        if (days) parts.push(`${days} day${days === 1 ? '' : 's'}`);
        if (hours) parts.push(`${hours} hour${hours === 1 ? '' : 's'}`);
        if (minutes || !parts.length) parts.push(`${minutes} minute${minutes === 1 ? '' : 's'}`);
        return parts.join(', ');
    };
    const alarmTime = value => {
        const epoch = Number(value || 0);
        return epoch ? new Date(epoch * 1000).toLocaleString([], {dateStyle: 'medium', timeStyle: 'short'}) : 'Never';
    };

    const renderPassword = data => {
        const node = ctx.$('#password-state');
        if (!node) return;
        node.textContent = data.password_required ? 'Enabled' : 'Not set';
        node.className = `badge ${data.password_required ? 'ok' : ''}`.trim();
    };

    const render = data => {
        ctx.setText('#diag-ip', available(data.ip_address));
        ctx.setText('#diag-hostname', available(data.hostname));
        ctx.setText('#diag-ssid', available(data.ssid));
        ctx.setText('#diag-interface', available(data.interface));
        ctx.setText('#diag-signal', data.wifi_signal_available
            ? `${data.wifi_signal_percent}% (${data.wifi_signal_dbm} dBm)` : 'Unavailable');
        ctx.setText('#diag-ntp', yesNo(data.ntp_synchronized));

        ctx.setText('#diag-product-version', available(data.product_version));
        ctx.setText('#diag-api-version', available(data.api_version));
        ctx.setText('#diag-compiled', available(data.compiled_at));
        ctx.setText('#diag-hardware', available(data.hardware_model));
        ctx.setText('#diag-os', available(data.os_pretty_name));
        const osRelease = [data.os_version_id, data.os_codename].filter(Boolean).join(' / ');
        ctx.setText('#diag-os-release', osRelease || 'Unavailable');
        ctx.setText('#diag-kernel', available(data.kernel_release));
        ctx.setText('#diag-architecture', available(data.architecture));
        ctx.setText('#diag-uptime', uptime(data.uptime_seconds));

        ctx.setText('#diag-inventory-id', available(data.inventory_id));
        ctx.setText('#diag-pi-serial', available(data.pi_serial));
        ctx.setText('#diag-board-revision', available(data.board_revision));
        ctx.setText('#diag-machine-id', available(data.machine_id));
        ctx.setText('#diag-cpu-signature', available(data.cpu_signature));

        ctx.setText('#diag-root-disk', available(data.root_disk));
        ctx.setText('#diag-root-device', available(data.root_device));
        ctx.setText('#diag-root-filesystem', available(data.root_filesystem));
        ctx.setText('#diag-root-state', data.root_device ? (data.root_read_only ? 'Read-only' : 'Read/write') : 'Unavailable');
        const storagePercent = data.storage_total_bytes
            ? ` (${((Number(data.storage_used_bytes || 0) / Number(data.storage_total_bytes)) * 100).toFixed(1)}%)`
            : '';
        ctx.setText('#diag-storage-used', `${bytes(data.storage_used_bytes)}${storagePercent}`);
        ctx.setText('#diag-storage-available', bytes(data.storage_free_bytes));
        ctx.setText('#diag-storage-total', bytes(data.storage_total_bytes));
        ctx.setText('#diag-day-images', directoryUsage(data.day_images_bytes, data.day_images_files));
        ctx.setText('#diag-bedtime-images', directoryUsage(data.bedtime_images_bytes, data.bedtime_images_files));
        ctx.setText('#diag-music-size', directoryUsage(data.music_bytes, data.music_files));
        ctx.setText('#diag-stories-size', directoryUsage(data.stories_bytes, data.stories_files));
        ctx.setText('#diag-fonts-size', directoryUsage(data.fonts_bytes, data.fonts_files));
        ctx.setText('#diag-boot-device', available(data.boot_device));
        ctx.setText('#diag-boot-filesystem', available(data.boot_filesystem));
        ctx.setText('#diag-boot-mount', available(data.boot_mount_point));

        ctx.setText('#diag-sd-device', available(data.sd_device));
        ctx.setText('#diag-sd-type', available(data.sd_type));
        ctx.setText('#diag-sd-name', available(data.sd_name));
        ctx.setText('#diag-sd-capacity', bytes(data.sd_capacity_bytes));
        ctx.setText('#diag-sd-manufacturer', available(data.sd_manufacturer_id));
        ctx.setText('#diag-sd-oem', available(data.sd_oem_id));
        ctx.setText('#diag-sd-serial', available(data.sd_serial));
        ctx.setText('#diag-sd-date', available(data.sd_manufacture_date));
        ctx.setText('#diag-sd-cid', available(data.sd_cid));
        const sdState = ctx.$('#sd-card-state');
        if (sdState) {
            sdState.textContent = data.sd_present ? 'Detected' : 'Unavailable';
            sdState.className = `badge ${data.sd_present ? 'ok' : 'warn'}`;
        }

        ctx.setText('#diag-api', working(data.api_healthy));
        ctx.setText('#diag-core', working(data.core_healthy));
        ctx.setText('#diag-oled', working(data.oled_ok));
        ctx.setText('#diag-touch', working(data.touch_ok));
        ctx.setText('#diag-temperature', data.cpu_temperature_c ? `${Number(data.cpu_temperature_c).toFixed(1)} °C` : 'Unavailable');
        ctx.setText('#diag-next-alarm', data.next_alarm_text || 'No alarm scheduled');
        ctx.setText('#diag-last-alarm', alarmTime(data.last_successful_alarm));

        const synchronized = Boolean(data.ntp_synchronized && data.system_time_valid);
        ctx.$('#system-time-warning')?.classList.toggle('hidden', synchronized);
        const network = ctx.$('#network-state');
        if (network) {
            network.textContent = data.ip_address ? 'Connected' : 'Unavailable';
            network.className = `badge ${data.ip_address ? 'ok' : 'warn'}`;
        }
    };

    const refresh = async () => {
        try {
            const [diagnostics, auth] = await Promise.all([
                ctx.json('/api/v1/diagnostics', {signal: ctx.signal}),
                ctx.json('/api/v1/auth/status', {signal: ctx.signal})
            ]);
            render(diagnostics);
            renderPassword(auth);
        } catch (error) {
            ctx.notice(error.message || 'Diagnostics could not be loaded', 'warn', 3000);
        }
    };

    ctx.on('submit', '#password-form', async (event, form) => {
        event.preventDefault();
        const password = form.elements.password.value;
        if (!password) {
            ctx.notice('Enter a password, or use Remove Password.', 'warn', 3000);
            return;
        }
        const button = event.submitter || form.querySelector('[type="submit"]');
        try {
            await ctx.update(form.action, {
                method: 'POST',
                body: new URLSearchParams({password}),
                button,
                busyText: 'Saving password...',
                done: 'Password saved',
                errorText: 'Password could not be saved',
                refreshStatus: false
            });
            form.reset();
            await refresh();
        } catch (_) {}
    });

    ctx.on('click', '#remove-password', async (_event, button) => {
        try {
            await ctx.update('/api/v1/auth/password', {
                method: 'POST',
                body: new URLSearchParams({password: ''}),
                button,
                busyText: 'Removing password...',
                done: 'Password removed',
                errorText: 'Password could not be removed',
                refreshStatus: false
            });
            ctx.$('#password-form')?.reset();
            await refresh();
        } catch (_) {}
    });

    ctx.on('submit', '#restore-form', async (event, form) => {
        event.preventDefault();
        if (!confirm('Restore this backup? Settings, images, and fonts will be replaced. Music and stories will remain unchanged.')) return;
        const button = form.querySelector('[type="submit"]');
        try {
            await ctx.update(form.action, {
                method: 'POST', body: new FormData(form), button,
                busyText: 'Restoring backup...', done: 'Backup restored', errorText: 'Backup could not be restored'
            });
            form.reset();
            await refresh();
        } catch (_) {}
    });

    ctx.on('submit', '#factory-reset-form', async (event, form) => {
        event.preventDefault();
        if (form.elements.confirm.value !== 'RESET') {
            ctx.notice('Type RESET to confirm.', 'warn', 3000);
            return;
        }
        if (!confirm('Factory reset the clock? This cannot be undone.')) return;
        const button = form.querySelector('[type="submit"]');
        try {
            await ctx.update(form.action, {
                method: 'POST', body: new URLSearchParams(new FormData(form)), button,
                busyText: 'Resetting clock...', done: 'Factory reset completed', errorText: 'Factory reset failed'
            });
            form.reset();
            await refresh();
        } catch (_) {}
    });

    await refresh();
    const timer = window.setInterval(() => refresh().catch(() => {}), 10000);
    ctx.signal.addEventListener('abort', () => window.clearInterval(timer), {once: true});
    return {refresh};
}
