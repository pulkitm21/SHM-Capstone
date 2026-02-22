import Navbar from "../components/Navbar/Navbar";
import { Outlet } from "react-router-dom";

function Layout() {
  return (
    <div className="app-shell">
      <Navbar />
      <div className="main">
        <Outlet />
      </div>
    </div>
  );
}

export default Layout;
