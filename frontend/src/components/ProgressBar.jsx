import React from 'react';

export default function ProgressBar({ value }) {
  return (
    <div className="progress">
      <progress value={value} max="100" />
      <span>{value}%</span>
    </div>
  );
}
