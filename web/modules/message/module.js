const MAX_MESSAGE_CHARS = 180;

export async function mount(ctx) {
    let previewTimer = null;
    let previewSequence = 0;
    let images = [];
    const preview = ctx.createOledPreview(ctx.$('#message-oled-canvas'));

    const setPreviewState = (text, type = '') => {
        const node = ctx.$('#message-preview-state');
        node.textContent = text;
        node.className = `badge ${type}`.trim();
    };

    const setTextHelp = (text, type = 'muted') => {
        const node = ctx.$('#message-text-help');
        node.textContent = text;
        node.className = `small ${type}`.trim();
    };

    const sendStatus = (text = '', type = '') => {
        const node = ctx.$('#message-send-status');
        node.textContent = text;
        node.className = `message-send-status ${type}`;
        node.classList.toggle('hidden', !text);
    };

    const renderPending = status => {
        if (!status?.message_pending) {
            ctx.setText('#message-pending', 'No message is scheduled.');
            return;
        }
        const seconds = Math.max(0, Number.parseInt(status.message_send_in_seconds || 0, 10));
        const epoch = Number.parseInt(status.message_scheduled_for || 0, 10);
        const exact = epoch > 0 ? new Date(epoch * 1000).toLocaleString() : '';
        ctx.setText('#message-pending', exact
            ? `Message scheduled for ${exact} (about ${seconds} seconds).`
            : `Message due in about ${seconds} seconds.`);
    };

    const renderStatus = status => {
        if (!status) return;
        preview.setColour(status.oled_color);
        renderPending(status);
    };

    const localDateTimeValue = date => {
        const pad = value => String(value).padStart(2, '0');
        return `${date.getFullYear()}-${pad(date.getMonth() + 1)}-${pad(date.getDate())}T${pad(date.getHours())}:${pad(date.getMinutes())}`;
    };

    const updateDeliveryFields = () => {
        const mode = ctx.$('#message-delivery-mode').value;
        ctx.$('#message-delay-fields').classList.toggle('hidden', mode !== 'delay');
        ctx.$('#message-time-fields').classList.toggle('hidden', mode !== 'time');
        ctx.setText('#message-submit', mode === 'now' ? 'Send Message' : 'Schedule Message');
        if (mode === 'time') {
            const input = ctx.$('#message-scheduled-time');
            input.min = localDateTimeValue(new Date(Date.now() + 60000));
            input.max = localDateTimeValue(new Date(Date.now() + 30 * 24 * 60 * 60 * 1000));
            if (!input.value) input.value = localDateTimeValue(new Date(Date.now() + 5 * 60000));
        }
    };

    const selectedPicture = () => {
        const option = ctx.$('#message-image-choice').selectedOptions[0];
        const file = option?.dataset.file || '';
        const bedtime = option?.dataset.bedtime === '1';
        const unavailable = option?.dataset.unavailable === '1';
        ctx.setValue('#message-image-file', file);
        ctx.setValue('#message-image-bedtime', bedtime ? '1' : '0');
        return {file, bedtime, unavailable};
    };

    const pictureAvailable = selected => {
        if (selected.unavailable) return false;
        if (selected.file) return true;
        return images.some(item => item.bedtime === selected.bedtime);
    };

    const previewPicture = selected => {
        if (selected.file) return selected;
        const example = images.find(item => item.bedtime === selected.bedtime);
        return {...selected, file: example?.file || ''};
    };

    const updateCounter = (message = '', type = 'muted') => {
        const length = ctx.$('#message-text').value.length;
        const suffix = message ? ` · ${message}` : '';
        setTextHelp(`${length} / ${MAX_MESSAGE_CHARS} characters${suffix}`, type);
    };

    const checkPreview = async () => {
        const text = ctx.$('#message-text').value || '';
        const selected = selectedPicture();
        const hasPicture = pictureAvailable(selected);
        const sequence = ++previewSequence;
        const submit = ctx.$('#message-submit');
        updateCounter();

        if (!text.trim() && !hasPicture) {
            submit.disabled = true;
            preview.clear();
            setPreviewState('Waiting');
            updateCounter('Choose a picture or type a message', 'warn-text');
            return;
        }

        const example = previewPicture(selected);
        const body = new URLSearchParams({
            image_file: example.file,
            image_bedtime: example.bedtime ? '1' : '0',
            message_text: text
        });
        setPreviewState('Updating');
        submit.disabled = true;
        try {
            const buffer = await ctx.binary('/api/v1/display/message/preview', {
                method: 'POST',
                body,
                headers: {
                    'Content-Type': 'application/x-www-form-urlencoded',
                    'Accept': 'application/octet-stream'
                },
                signal: ctx.signal
            });
            if (ctx.signal.aborted || sequence !== previewSequence) return;
            preview.draw(buffer);
            setPreviewState('Exact', 'ok');
            updateCounter();
            submit.disabled = false;
        } catch (error) {
            if (ctx.signal.aborted || sequence !== previewSequence) return;
            preview.clear();
            setPreviewState('Needs shortening', 'warn');
            updateCounter(error.message || 'Could not build the clock preview', 'warn-text');
        }
    };

    const schedulePreview = () => {
        updateCounter();
        clearTimeout(previewTimer);
        previewTimer = window.setTimeout(checkPreview, 180);
    };

    const loadImages = async () => {
        const [dayResult, bedtimeResult] = await Promise.allSettled([
            ctx.json('/api/v1/assets/images?all=1'),
            ctx.json('/api/v1/assets/bedtime-images?all=1')
        ]);
        const dayImages = dayResult.status === 'fulfilled' ? (dayResult.value.images || []) : [];
        const bedtimeImages = bedtimeResult.status === 'fulfilled' ? (bedtimeResult.value.images || []) : [];
        images = [
            ...dayImages.map(item => ({...item, bedtime: false})),
            ...bedtimeImages.map(item => ({...item, bedtime: true}))
        ];

        const dayOptions = dayImages.map(imageAsset =>
            `<option data-file="${ctx.html(imageAsset.file)}" data-bedtime="0">${ctx.html(imageAsset.title || imageAsset.file)}</option>`
        ).join('');
        const bedtimeOptions = bedtimeImages.map(imageAsset =>
            `<option data-file="${ctx.html(imageAsset.file)}" data-bedtime="1">${ctx.html(imageAsset.title || imageAsset.file)}</option>`
        ).join('');
        const randomOptions = [
            dayImages.length ? '<option data-file="" data-bedtime="0">Random Day Image</option>' : '',
            bedtimeImages.length ? '<option data-file="" data-bedtime="1">Random Bedtime Image</option>' : ''
        ].join('');

        ctx.$('#message-image-choice').innerHTML = `
            ${randomOptions || '<option data-file="" data-bedtime="0" data-unavailable="1">No images uploaded</option>'}
            ${dayOptions ? `<optgroup label="Day Images">${dayOptions}</optgroup>` : ''}
            ${bedtimeOptions ? `<optgroup label="Bedtime Images">${bedtimeOptions}</optgroup>` : ''}`;
        selectedPicture();
    };

    ctx.status.subscribe(renderStatus);
    ctx.on('input', '#message-text', schedulePreview);
    ctx.on('change', '#message-image-choice', schedulePreview);
    ctx.on('change', '#message-delivery-mode', updateDeliveryFields);
    ctx.on('click', '#message-clear', () => {
        ctx.setValue('#message-text', '');
        sendStatus('Message draft cleared.', 'ok');
        schedulePreview();
        ctx.$('#message-text').focus();
    });
    ctx.on('submit', '#message-form', async (event, form) => {
        event.preventDefault();
        selectedPicture();
        const button = ctx.$('#message-submit');
        const mode = ctx.$('#message-delivery-mode').value;
        const delay = mode === 'delay' ? Number.parseInt(ctx.$('#message-delay').value || '10', 10) : 0;
        let scheduledAt = 0;
        let scheduledLabel = '';
        if (mode === 'time') {
            const selected = new Date(ctx.$('#message-scheduled-time').value);
            if (!Number.isFinite(selected.getTime()) || selected.getTime() <= Date.now()) {
                sendStatus('Choose a future date and time.', 'warn');
                return;
            }
            scheduledAt = Math.floor(selected.getTime() / 1000);
            scheduledLabel = selected.toLocaleString();
        }
        const body = new URLSearchParams(new FormData(form));
        body.set('format', 'json');
        body.set('delay_seconds', String(delay));
        body.set('scheduled_at', String(scheduledAt));
        sendStatus();
        const scheduled = delay > 0 || scheduledAt > 0;
        try {
            await ctx.update(form.action, {
                method: 'POST',
                body,
                button,
                busyText: scheduled ? 'Scheduling message...' : 'Sending message...',
                done: scheduled ? 'Message scheduled' : 'Message shown on OLED',
                errorText: scheduled ? 'Message could not be scheduled' : 'Message could not be shown'
            });
            ctx.setValue('#message-text', '');
            await checkPreview();
            sendStatus(scheduledAt > 0
                ? `Message scheduled for ${scheduledLabel}.`
                : delay > 0 ? `Message scheduled for ${delay} seconds from now.`
                : 'Sent to the clock. Ready for the next message.', 'ok');
            ctx.$('#message-text').focus();
        } catch (error) {
            sendStatus(error.message || 'Message was not sent.', 'warn');
        }
    });

    ctx.signal.addEventListener('abort', () => clearTimeout(previewTimer), {once: true});

    preview.clear();
    setPreviewState('Loading');
    updateDeliveryFields();
    updateCounter();
    await Promise.all([ctx.status.refresh(), loadImages()]);
    schedulePreview();
}
