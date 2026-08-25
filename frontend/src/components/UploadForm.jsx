import React, { useRef, useState } from 'react';
import { uploadFile } from '../api/nervfsApi';
import ProgressBar from './ProgressBar';

const isValidName = (name) =>
  name.length > 0 &&
  name.length <= 255 &&
  !name.includes('/') &&
  name !== '.' &&
  name !== '..' &&
  !/\0/.test(name);

export default function UploadForm({ onUploaded, onNotify }) {
  const inputRef = useRef(null);
  const [progress, setProgress] = useState(0);
  const [uploading, setUploading] = useState(false);
  const [error, setError] = useState('');

  async function handleChange(event) {
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }

    if (!isValidName(file.name)) {
      const errMsg = 'Invalid filename.';
      setError(errMsg);
      if (onNotify) {
        onNotify('error', 'Upload Error', errMsg);
      }
      event.target.value = '';
      return;
    }

    setError('');
    setProgress(0);
    setUploading(true);

    try {
      const { serverMs, clientMs } = await uploadFile(file, setProgress);
      setProgress(100);

      if (onNotify) {
        onNotify(
          'success',
          'Upload Complete',
          `Successfully uploaded ${file.name}`,
          serverMs,
          clientMs
        );
      }

      await onUploaded();
      event.target.value = '';
    } catch (err) {
      const errMsg = err.response?.data?.error || err.message || 'Upload failed.';
      setError(errMsg);
      if (onNotify) {
        onNotify('error', 'Upload Failed', errMsg);
      }
    } finally {
      setUploading(false);
    }
  }

  return (
    <section className="toolbar" aria-label="Upload file">
      <input ref={inputRef} type="file" onChange={handleChange} disabled={uploading} />
      <button type="button" onClick={() => inputRef.current?.click()} disabled={uploading}>
        Upload
      </button>
      {uploading && <ProgressBar value={progress} />}
      {error && <p className="error">{error}</p>}
    </section>
  );
}