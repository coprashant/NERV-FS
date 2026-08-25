import React, { useEffect, useRef, useState } from 'react';
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

export default function FileList({ files, onChanged, onNotify }) {
  const [permissionsFile, setPermissionsFile] = useState(null);
  const [busyName, setBusyName] = useState('');
  const [error, setError] = useState('');
  const [openMenu, setOpenMenu] = useState('');
  const menuRef = useRef(null);

  useEffect(() => {
    function handleOutsideClick(event) {
      if (menuRef.current && !menuRef.current.contains(event.target)) {
        setOpenMenu('');
      }
    }

    document.addEventListener('mousedown', handleOutsideClick);
    return () => document.removeEventListener('mousedown', handleOutsideClick);
  }, []);

  async function handleDelete(name) {
    setOpenMenu('');
    setBusyName(name);
    setError('');

    try {
      const { serverMs, clientMs } = await deleteFile(name);

      if (onNotify) {
        onNotify(
          'info',
          'File Deleted',
          `Successfully deleted ${name}`,
          serverMs,
          clientMs
        );
      }

      await onChanged();
    } catch (err) {
      const errMsg = err.response?.data?.error || err.message || 'Delete failed.';
      setError(errMsg);
      if (onNotify) {
        onNotify('error', 'Delete Failed', errMsg);
      }
    } finally {
      setBusyName('');
    }
  }

  function openPermissions(file) {
    setOpenMenu('');
    setPermissionsFile(file);
  }

  function triggerDownload(name) {
    setOpenMenu('');
    downloadFile(name);

    if (onNotify) {
      onNotify('info', 'Download Started', `Downloading ${name}...`);
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
            <th className="modeCol">Mode</th>
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
                <td className="modeCol">{formatMode(file.mode)}</td>
                <td className="actionsCell">
                  <div className="actions">
                    <button type="button" onClick={() => triggerDownload(file.name)}>
                      Download
                    </button>
                    <button type="button" onClick={() => openPermissions(file)}>
                      Mode
                    </button>
                    <button
                      type="button"
                      onClick={() => handleDelete(file.name)}
                      disabled={busyName === file.name}
                    >
                      Delete
                    </button>
                  </div>

                  <div className="actionsMenu" ref={openMenu === file.name ? menuRef : null}>
                    <button
                      type="button"
                      className="kebabButton"
                      onClick={() => setOpenMenu(openMenu === file.name ? '' : file.name)}
                      aria-label="Actions"
                      aria-expanded={openMenu === file.name}
                    >
                      &#8942;
                    </button>

                    {openMenu === file.name && (
                      <div className="actionsDropdown" role="menu">
                        <p className="dropdownModeValue">Mode: {formatMode(file.mode)}</p>
                        <button type="button" onClick={() => triggerDownload(file.name)}>
                          Download
                        </button>
                        <button type="button" onClick={() => openPermissions(file)}>
                          Mode
                        </button>
                        <button
                          type="button"
                          onClick={() => handleDelete(file.name)}
                          disabled={busyName === file.name}
                        >
                          Delete
                        </button>
                      </div>
                    )}
                  </div>
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
          onNotify={onNotify}
        />
      )}
    </section>
  );
}