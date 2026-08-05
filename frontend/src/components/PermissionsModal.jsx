import React, { useState } from 'react';
import { setPermissions } from '../api/nervfsApi';

export default function PermissionsModal({ file, onClose, onSaved }) {
  const [modeText, setModeText] = useState(file.mode.toString(8).padStart(4, '0'));
  const [saving, setSaving] = useState(false);
  const [error, setError] = useState('');

  async function handleSubmit(event) {
    event.preventDefault();

    if (!/^[0-7]{3,4}$/.test(modeText)) {
      setError('Mode must be a 3 or 4 digit octal value.');
      return;
    }

    setSaving(true);
    setError('');

    try {
      await setPermissions(file.name, Number.parseInt(modeText, 8));
      await onSaved();
      onClose();
    } catch (err) {
      setError(err.response?.data?.error || err.message || 'Could not update permissions.');
    } finally {
      setSaving(false);
    }
  }

  return (
    <div className="modalBackdrop" role="presentation" onMouseDown={onClose}>
      <form className="modal" onSubmit={handleSubmit} onMouseDown={(event) => event.stopPropagation()}>
        <div className="modalHeader">
          <h2>Permissions</h2>
          <button type="button" className="iconButton" onClick={onClose} aria-label="Close">
            x
          </button>
        </div>
        <p className="filename">{file.name}</p>
        <label>
          Mode
          <input
            value={modeText}
            onChange={(event) => setModeText(event.target.value.trim())}
            inputMode="numeric"
            maxLength="4"
            autoFocus
          />
        </label>
        {error && <p className="error">{error}</p>}
        <div className="modalActions">
          <button type="button" onClick={onClose} disabled={saving}>
            Cancel
          </button>
          <button type="submit" disabled={saving}>
            Save
          </button>
        </div>
      </form>
    </div>
  );
}
