import { NavLink, useNavigate } from "react-router-dom";
import "./Navbar.css";

interface NavItem {
  id: string;
  label: string;
  icon: string;
  to: string;
}

const navItems: NavItem[] = [
  { id: "home", label: "Home", icon: "🏠︎", to: "/" },
  {id: "sensor-management", label: "Sensor Management", icon: "🛠️", to: "/sensor-management" },
  { id: "fault-log", label: "Fault Log", icon: "⚠️", to: "/fault-log" },
  { id: "export", label: "Export", icon: "💾", to: "/export" },
];

const Navbar = () => {
  const navigate = useNavigate();

  function handleLogout() {
    sessionStorage.removeItem("isAuth");
    navigate("/login", { replace: true });
  }

  return (
    <nav className="navbar">
      <div className="navbar-title-block">
        <h1>Cape Scott, BC</h1>
      </div>

      <ul className="navbar-menu">
        {navItems.map((item) => (
          <li key={item.id}>
            <NavLink
              to={item.to}
              end={item.to === "/"} 
              className={({ isActive }) =>
                `navbar-item ${isActive ? "active" : ""}`
              }
            >
              <span className="navbar-icon">{item.icon}</span>
              <span className="navbar-label">{item.label}</span>
            </NavLink>
          </li>
        ))}
      </ul>

      <div className="navbar-footer">
        <button className="logout-button" onClick={handleLogout}>
          Logout
        </button>
      </div>
    </nav>
  );
};

export default Navbar;