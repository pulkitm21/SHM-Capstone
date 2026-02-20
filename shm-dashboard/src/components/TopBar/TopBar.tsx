import './TopBar.css';
import { useNavigate } from "react-router-dom";

const TopBar = () => {
  const navigate = useNavigate();

  function handleLogout() {
    sessionStorage.removeItem("isAuth");
    navigate("/login", { replace: true });
  }

  return (
    <header className="header">
      <div className="header-buttons">
        <button className="logout-button" onClick={handleLogout}>
          Logout
        </button>
      </div>
    </header>
  );
};

export default TopBar;
