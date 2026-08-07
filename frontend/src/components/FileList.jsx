import React, { useState } from 'react';
import { deleteFile, downloadFile } from '../api/nervfsApi';
import PermissionsModal from './PermissionsModal';

const formatSize = (value) => {
  const size = Number(value);
  if (size >= 1e9) return `${(size / 1e9).toFixed(1)} GB`;
  if (size >= 1e6) return `${(size / 1e6).toFixed(1)} MB`;
  if (size >= 1e3) return `${(size / 1e3).toFixed(1)} KB`;
  return `${size} B`;
};

const formatMode = (mode) => mode.toString(8).padStart(4, '0');

export default function FileList({ files, onChanged }) {
  const [permissionsFile, setPermissionsFile] = useState(null);
  const [busyName, setBusyName] = useState('');
  const [error, setError] = useState('');

  async function handleDelete(name) {
    setBusyName(name);
    setError('');

    try {
      await deleteFile(name);
      await onChanged();
    } catch (err) {
      setError(err.response?.data?.error || err.message || 'Delete failed.');
    } finally {
      setBusyName('');
    }
  }

  return (
    <section className="fileSection">
      {error && <p className="error">{error}</p>}
      <table>
        <thead>
          <tr>
            <th>Name</th>
            <th>Size</th>
            <th>Mode</th>
            <th>Actions</th>
          </tr>
        </thead>
        <tbody>
          {files.length === 0 ? (
            <tr>
              <td colSpan="4" className="empty">
                No files
              </td>
            </tr>
          ) : (
            files.map((file) => (
              <tr key={file.name}>
                <td className="nameCell">{file.name}</td>
                <td>{formatSize(file.size)}</td>
                <td>{formatMode(file.mode)}</td>
                <td className="actions">
                  <button type="button" onClick={() => downloadFile(file.name)}>
                    Download
                  </button>
                  <button type="button" onClick={() => setPermissionsFile(file)}>
                    Mode
                  </button>
                  <button
                    type="button"
                    onClick={() => handleDelete(file.name)}
                    disabled={busyName === file.name}
                  >
                    Delete
                  </button>
                </td>
              </tr>
            ))
          )}
        </tbody>
      </table>

      {permissionsFile && (
        <PermissionsModal
          file={permissionsFile}
          onClose={() => setPermissionsFile(null)}
          onSaved={onChanged}
        />
      )}
    </section>
  );
}
