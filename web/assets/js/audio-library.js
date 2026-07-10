export function formatAudioDuration(value) {
    const seconds = Math.max(0, Math.round(Number(value) || 0));
    if (!seconds) return '';
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const remaining = seconds % 60;
    return hours
        ? `${hours}:${String(minutes).padStart(2, '0')}:${String(remaining).padStart(2, '0')}`
        : `${minutes}:${String(remaining).padStart(2, '0')}`;
}

export function formatAudioBytes(value) {
    const bytes = Number(value) || 0;
    if (!bytes) return '';
    if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GiB`;
    if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
    return `${Math.round(bytes / 1024)} KiB`;
}

export function audioTrackFacts(track, options = {}) {
    const facts = [];
    const duration = formatAudioDuration(track.duration_seconds);
    if (duration) facts.push(`${track.duration_estimated ? 'Approx. ' : ''}${duration}`);
    if (track.bitrate_kbps) facts.push(`${track.bitrate_kbps} kbps ${track.bitrate_mode || ''}`.trim());
    if (track.sample_rate_hz)
        facts.push(`${(track.sample_rate_hz / 1000).toFixed(track.sample_rate_hz % 1000 ? 1 : 0)} kHz`);
    if (options.channels !== false && track.channels)
        facts.push(track.channels === 1 ? 'Mono' : track.channels === 2 ? 'Stereo' : `${track.channels} channels`);
    if (options.layer !== false && track.mpeg_layer) facts.push(`MPEG Layer ${track.mpeg_layer}`);
    const size = formatAudioBytes(track.file_size_bytes);
    if (size) facts.push(size);
    return facts;
}
