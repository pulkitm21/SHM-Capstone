import Navbar from "../components/Navbar/Navbar";
import Topbar from "../components/TopBar/TopBar";
import "./Layout.css";
import { Outlet } from "react-router-dom";

function Layout() {
  return (
    <div className="app-shell">
      <Navbar />
      <div className="main">
        <Topbar />
        <Outlet />
      </div>
    </div>
  );
}

export default Layout;
