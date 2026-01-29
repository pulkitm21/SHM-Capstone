import { useState } from 'react';
import './TopBar.css';
import { useNavigate } from "react-router-dom";


const TopBar = () => {
  const [searchQuery, setSearchQuery] = useState('');
  const navigate = useNavigate();

  /* Handle User Logout. Needs update based on auth implementation */
  function handleLogout() {
    sessionStorage.removeItem("isAuth");
    navigate("/login", { replace: true });
  }


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

      <div className="header-buttons">
        <button className="logout-button" onClick={handleLogout}>
          Logout
        </button>
      </div>

    </header>
  );
};

export default TopBar;