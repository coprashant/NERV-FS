import axios from 'axios';

const BASE = import.meta.env.VITE_NERVFS_BRIDGE_URL || 'http://localhost:4000';

export const listFiles = () =>
  axios.get(`${BASE}/files`).then((response) => response.data);

export const uploadFile = (file, onProgress) => {
  const form = new FormData();
  form.append('file', file);

  return axios.post(`${BASE}/files`, form, {
    onUploadProgress: (event) => {
      if (onProgress && event.total) {
        onProgress(Math.round((event.loaded * 100) / event.total));
      }
    },
  });
};

export const downloadFile = (name) => {
  window.location.href = `${BASE}/files/${encodeURIComponent(name)}`;
};

export const deleteFile = (name) =>
  axios.delete(`${BASE}/files/${encodeURIComponent(name)}`);

export const setPermissions = (name, mode) =>
  axios.patch(`${BASE}/files/${encodeURIComponent(name)}/permissions`, { mode });
