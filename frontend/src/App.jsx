import React, { useCallback, useEffect, useState } from 'react';
import { listFiles } from './api/nervfsApi';
import FileList from './components/FileList';
import UploadForm from './components/UploadForm';

export default function App() {
  const [files, setFiles] = useState([]);
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState('');

  const refreshFiles = useCallback(async () => {
    try {
      setError('');
      const data = await listFiles();
      setFiles(data.files || []);
    } catch (err) {
      setError(err.response?.data?.error || err.message || 'Could not load files.');
    } finally {
      setLoading(false);
    }
  }, []);

  useEffect(() => {
    refreshFiles();
  }, [refreshFiles]);

  return (
    <main className="appShell">
      <header>
        <div>
          <h1>NERV-FS</h1>
          <p>Remote file store</p>
        </div>
        <button type="button" onClick={refreshFiles} disabled={loading}>
          Refresh
        </button>
      </header>

      <UploadForm onUploaded={refreshFiles} />
      {error && <p className="error">{error}</p>}
      {loading ? <p className="muted">Loading files...</p> : <FileList files={files} onChanged={refreshFiles} />}
    </main>
  );
}
