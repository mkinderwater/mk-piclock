export async function mountImageLibrary(ctx, options) {
    let page = 1;
    const root = ctx.root;
    const previewBlobUrls = new Set();
    const titleCase = value => String(value || '').replace(/^./, letter => letter.toUpperCase());
    const releasePreviewUrls = () => {
        previewBlobUrls.forEach(url => URL.revokeObjectURL(url));
        previewBlobUrls.clear();
    };
    ctx.signal.addEventListener('abort', releasePreviewUrls, {once: true});

    root.innerHTML = `
        <section class="panel image-library-module">
            <div class="card">
                <h2>${ctx.html(options.heading)}</h2>
                <p class="page-intro">${ctx.html(options.description)}</p>
                <form method="POST" action="${ctx.html(options.uploadUrl)}" enctype="multipart/form-data"
                      class="upload-card"
                      data-busy-text="Adding ${ctx.html(options.labelPlural)}..."
                      data-success-text="${ctx.html(titleCase(options.labelPlural))} added"
                      data-error-text="The images could not be added">
                    <label>Choose PNG files</label>
                    <input type="file" name="files" accept="image/png,.png" multiple required>
                    <button class="btn" type="submit">Add ${ctx.html(titleCase(options.labelPlural))}</button>
                </form>
                <div class="field-row image-library-summary">
                    <div>
                        <label for="image-count">Images in this library</label>
                        <input id="image-count" type="text" value="Loading..." readonly>
                    </div>
                    <div id="image-page-field">
                        <label for="image-page-select">Page</label>
                        <select id="image-page-select"></select>
                    </div>
                </div>
                <details class="danger-details">
                    <summary>Library options</summary>
                    <p class="small">Download a ZIP containing only the original PNG files.</p>
                    <button class="btn alt small-btn" id="download-all-images" type="button">${ctx.html(options.downloadLabel || 'Download All PNGs')}</button>
                    <p class="small library-delete-note">Use this only when you want to remove every image in this section.</p>
                    <button class="btn danger small-btn" type="button" id="delete-all-images">Delete All ${ctx.html(titleCase(options.labelPlural))}</button>
                </details>
            </div>
            <div id="image-list" class="cards"></div>
        </section>`;

    const renderCards = async images => {
        releasePreviewUrls();
        const target = ctx.$('#image-list');
        if (!images.length) {
            target.innerHTML = `<div class="card"><p>${ctx.html(options.emptyText)}</p></div>`;
            return;
        }
        target.innerHTML = images.map((image, index) => `
            <div class="card image-card" data-file="${ctx.html(image.file)}">
                <div class="image-card-layout">
                    ${image.preview_url
                        ? `<img class="image-preview-img" data-preview-index="${index}" alt="${ctx.html(image.title || image.file)}">`
                        : '<div class="image-preview-missing">IMAGE</div>'}
                    <div>
                        <h2>${ctx.html(image.title || image.file)}</h2>
                        <div class="image-actions">
                            <button class="btn danger" type="button" data-image-delete="${ctx.html(image.file)}" data-image-title="${ctx.html(image.title || image.file)}">Delete</button>
                        </div>
                        <details class="file-details">
                            <summary>File details</summary>
                            <p class="small mono ${image.source_png ? '' : 'muted'}">${image.source_png ? `PNG: ${ctx.html(image.source_png)}` : 'Original PNG not available'}</p>
                            <p class="small mono">Clock file: ${ctx.html(image.file)}</p>
                        </details>
                    </div>
                </div>
            </div>`).join('');

        await Promise.all(images.map(async (image, index) => {
            if (!image.preview_url || ctx.signal.aborted) return;
            const node = target.querySelector(`[data-preview-index="${index}"]`);
            if (!node) return;
            try {
                const buffer = await ctx.binary(image.preview_url, {signal: ctx.signal});
                if (ctx.signal.aborted || !node.isConnected) return;
                const blobUrl = URL.createObjectURL(new Blob([buffer], {type: 'image/png'}));
                previewBlobUrls.add(blobUrl);
                node.src = blobUrl;
            } catch (_) {
                if (!ctx.signal.aborted && node.isConnected) {
                    const missing = document.createElement('div');
                    missing.className = 'image-preview-missing';
                    missing.textContent = 'IMAGE';
                    node.replaceWith(missing);
                }
            }
        }));
    };

    const refresh = async () => {
        try {
            const data = await ctx.json(`${options.listUrl}?page=${page}`);
            page = data.page || 1;
            const pageCount = data.max_page || 1;
            ctx.$('#image-page-select').innerHTML = Array.from({length: pageCount}, (_, index) => {
                const value = index + 1;
                return `<option value="${value}"${value === page ? ' selected' : ''}>${value} of ${pageCount}</option>`;
            }).join('');
            ctx.$('#image-page-field').classList.toggle('hidden', pageCount <= 1);
            ctx.setValue('#image-count', data.count ? `${data.count} image${data.count === 1 ? '' : 's'}` : 'No images');
            await renderCards(data.images || []);
        } catch (_) {
            ctx.setValue('#image-count', 'Could not load');
            ctx.$('#image-list').innerHTML = `<div class="card"><p>${ctx.html(titleCase(options.labelPlural))} could not be loaded.</p></div>`;
        }
    };

    ctx.on('change', '#image-page-select', (_, select) => {
        page = Number.parseInt(select.value || '1', 10);
        refresh();
    });

    ctx.on('click', '[data-image-delete]', async (_, button) => {
        const file = button.dataset.imageDelete;
        const title = button.dataset.imageTitle || file;
        if (!file || !confirm(`Delete “${title}”?`)) return;
        try {
            await ctx.update(options.deleteUrl, {
                method: 'POST',
                body: new URLSearchParams({file, format: 'json'}),
                button,
                busyText: `Deleting ${title}...`,
                done: `${title} deleted`,
                errorText: `${title} could not be deleted`
            });
            await refresh();
        } catch (_) {}
    });

    ctx.on('click', '#download-all-images', async (_, button) => {
        const label = options.downloadLabel || 'Download All PNGs';
        try {
            ctx.busy(button, true, 'Preparing download...');
            ctx.notice('Preparing image archive...', 'busy');
            const buffer = await ctx.binary(options.downloadUrl);
            const blobUrl = URL.createObjectURL(new Blob([buffer], {type: 'application/zip'}));
            const link = document.createElement('a');
            link.href = blobUrl;
            link.download = options.downloadFilename || 'mk-piclock-images-png.zip';
            document.body.appendChild(link);
            link.click();
            link.remove();
            window.setTimeout(() => URL.revokeObjectURL(blobUrl), 1000);
            ctx.notice(`${label} ready`, 'ok', 1800);
        } catch (error) {
            ctx.notice(error.message || 'The image archive could not be downloaded.', 'warn', 3500);
        } finally {
            ctx.busy(button, false);
        }
    });

    ctx.on('click', '#delete-all-images', async (_, button) => {
        if (!confirm(`Delete all ${options.labelPlural}? This cannot be undone.`)) return;
        try {
            await ctx.update(options.deleteAllUrl, {
                method: 'POST',
                body: new URLSearchParams({format: 'json'}),
                button,
                busyText: `Deleting all ${options.labelPlural}...`,
                done: `All ${options.labelPlural} deleted`,
                errorText: `The ${options.labelPlural} library could not be cleared`
            });
            page = 1;
            await refresh();
        } catch (_) {}
    });

    await refresh();
    return {refresh};
}
