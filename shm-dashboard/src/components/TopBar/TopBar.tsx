import { useState } from 'react';
import './TopBar.css';

const TopBar = () => {
  const [searchQuery, setSearchQuery] = useState('');

  return (
    <header className="header">
      <div className="search-container">
        <input
          type="text"
          placeholder="Search dashboard..."
          value={searchQuery}
          onChange={(e) => setSearchQuery(e.target.value)}
          className="search-input"
        />
      </div>
    </header>
  );
};

export default TopBar;