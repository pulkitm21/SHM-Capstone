import Tiles from "../../components/Tiles/Tiles";

export default function Home() {
  const tiles = [
    { id: "1", title: "Template Tile", count: 1, color: "#e3f2fd", icon: "🧩" },
  ];

  return <Tiles tiles={tiles} />;
}
