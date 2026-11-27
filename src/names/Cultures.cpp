// Naming cultures: street and district words (Settlement's cultures and mexican) and the vocabularies places and
// businesses are named from: mexican, english, french, latin and scifi.

#include "names/Names.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <stdexcept>

namespace lila::names
{
namespace
{
using Words = std::vector<const char*>;

/** Words a culture builds street and district names from: roots, and per class the words that go before or after them. */
struct Culture
{
    const char* name;
    Words roots;
    std::array<Words, 5> classWords; // arterial, collector, local, alley, path
    bool wordFirst = false;          // "Rue X" rather than "X Street"
    Words districtRoots;
    Words districtEnds; // joined onto a root ("Ash" + "ford"); empty: the root alone
    const char* districtPrefix = "";
    const char* vocabulary = "english";
};

const std::vector<Culture>& all()
{
    static const std::vector<Culture> c = {
        {"oldEnglish",
         {"Mill", "Church", "Market", "Bridge", "Castle", "Abbey", "Kings", "Queens", "Bishops", "Friars", "Tanners", "Weavers",
          "Smiths", "Coopers", "Bakers", "Cheap", "Wool", "Salt", "Fish", "Horse", "Sheep", "Barley", "Oak", "Ash", "Elm",
          "Thorn", "Holly", "Willow", "Hazel", "Rose", "Swan", "Crown", "Mint", "Cross", "Chapel", "Priory", "Manor", "Hall",
          "Green", "Moor", "Fen", "Marsh", "Brook", "Well", "Spring", "Stone", "Flint", "Chalk", "Hay", "Corn", "Malt", "Bell",
          "Lamb", "Fox", "Hart", "Raven", "Pudding", "Silver", "Copper", "Iron", "Water", "Wind", "High", "Broad", "Long",
          "Narrow", "Old", "Shambles", "Butchers", "Glovers", "Potters", "Ropers", "Dyers", "Mercers", "Chandlers"},
         {Words{"Street", "Road", "Way", "Gate"}, Words{"Street", "Lane", "Row", "Gate"}, Words{"Lane", "Row", "Close", "Yard", "Court"},
          Words{"Alley", "Passage", "Wynd", "Ginnel", "Snicket"}, Words{"Walk", "Path", "Steps", "Twitten"}},
         false,
         {"North", "South", "East", "West", "Ash", "Oak", "Mill", "Kings", "Church", "Market", "Stone", "Thorn", "Wool", "Salt",
          "Brook", "Elm", "Hay", "Fox", "Raven", "Bell", "Bishops", "Holly", "Swan", "Marsh"},
         {"gate", "field", "ford", "bury", "wick", "ton", "ham", "stead", "cross", "moor", "dale", "side", "wood", "end", "hythe"}},
        {"latin",
         {"Appia", "Aurelia", "Flaminia", "Iulia", "Claudia", "Sacra", "Nova", "Lata", "Recta", "Salaria", "Tiburtina",
          "Ostiensis", "Cornelia", "Valeria", "Aemilia", "Cassia", "Domitia", "Augusta", "Minervae", "Martis", "Iovis",
          "Veneris", "Fortunae", "Mercurii", "Saturni", "Dianae", "Apollinis", "Vestae", "Fori", "Portae", "Fontis", "Pontis",
          "Castrorum", "Fabrorum", "Pistorum", "Lanariorum", "Vinariorum", "Argentariorum", "Tuscus", "Iugarius", "Patricius"},
         {Words{"Via"}, Words{"Via", "Vicus"}, Words{"Vicus"}, Words{"Angiportus", "Semita"}, Words{"Semita", "Clivus", "Scalae"}},
         true,
         {"Palatium", "Aventinus", "Caelius", "Esquiliae", "Viminalis", "Quirinalis", "Subura", "Velabrum", "Carinae",
          "Campus Martius", "Forum", "Emporium", "Transtiberim", "Collis Hortulorum", "Insula", "Circus", "Vicus Longus",
          "Argiletum", "Ianiculum", "Pincius"},
         {},
         "",
         "latin"},
        {"french",
         {"de la Paix", "du Marché", "de l'Église", "des Tanneurs", "du Château", "de la Fontaine", "des Fleurs", "du Moulin",
          "Saint-Jacques", "Saint-Martin", "Saint-Michel", "Saint-Denis", "Saint-Honoré", "Saint-Antoine", "des Orfèvres",
          "de la Poste", "du Pont", "des Écoles", "Victor Hugo", "Voltaire", "Molière", "des Lilas", "des Peupliers",
          "du Faubourg", "de la Gare", "des Halles", "de la République", "de la Liberté", "Lafayette", "Colbert", "Pasteur",
          "des Arts", "du Temple", "des Jardins", "de la Croix", "du Puits", "des Remparts", "des Carmes", "des Bouchers",
          "de la Monnaie", "des Cordeliers"},
         {Words{"Boulevard", "Avenue"}, Words{"Rue", "Avenue"}, Words{"Rue"}, Words{"Ruelle", "Passage", "Impasse"},
          Words{"Allée", "Sentier", "Escalier"}},
         true,
         {"Saint-Germain", "Latin", "du Marais", "des Halles", "de la Gare", "du Port", "Saint-Paul", "des Batignolles",
          "de Montmartre", "Saint-Roch", "du Temple", "de la Cité", "Saint-Victor", "des Ternes", "de Grenelle", "de Belleville",
          "Saint-Lazare", "de la Roquette", "des Invalides", "de l'Arsenal"},
         {},
         "Quartier ",
         "french"},
        {"spanish",
         {"Mayor", "Real", "del Sol", "de la Luna", "del Carmen", "de San Juan", "de Santiago", "de la Paz", "del Rosario",
          "de los Reyes", "de las Flores", "del Mercado", "de la Iglesia", "del Río", "de la Fuente", "del Puente", "de Alcalá",
          "de Toledo", "de Cervantes", "de la Constitución", "de la Merced", "de San Francisco", "de los Olivos", "del Pilar",
          "de la Victoria", "del Castillo", "de la Plata", "de los Herreros", "de la Cruz", "del Puerto", "de las Huertas",
          "de los Tintes", "del Agua", "de la Feria"},
         {Words{"Avenida", "Paseo"}, Words{"Calle"}, Words{"Calle"}, Words{"Callejón", "Pasaje"}, Words{"Sendero", "Escalinata"}},
         true,
         {"de San Juan", "del Carmen", "de Santa Cruz", "Alto", "Bajo", "del Puerto", "de la Merced", "de Triana", "del Albaicín",
          "de la Catedral", "de San Pedro", "de la Judería", "de San Lorenzo", "del Castillo", "de las Huertas", "de San Marcos",
          "de la Magdalena", "del Pilar"},
         {},
         "Barrio ",
         "mexican"},
        {"usGrid",
         {"Main", "Oak", "Maple", "Elm", "Pine", "Cedar", "Washington", "Lincoln", "Jefferson", "Madison", "Franklin", "Park",
          "Lake", "Hill", "Church", "Center", "Market", "Union", "Liberty", "Spring", "Walnut", "Cherry", "Chestnut", "Broadway",
          "Highland", "Ridge", "River", "Mill", "Jackson", "Adams", "Grant", "Monroe", "Locust", "Sycamore", "Hickory"},
         {Words{"Avenue", "Boulevard"}, Words{"Street", "Avenue"}, Words{"Street", "Drive", "Place"}, Words{"Alley"}, Words{"Walk", "Trail"}},
         false,
         {"Midtown", "Uptown", "Old Town", "Riverside", "Eastside", "Westside", "Northside", "Southside", "Hillcrest",
          "Lakeview", "Fairview", "Oak Park", "Mill District", "Arts District", "Warehouse District", "University Heights",
          "Brookside", "Greenwood", "Elmwood", "Ironbound"},
         {}},
        {"sector", {}, {Words{"Trunk"}, Words{"Corridor"}, Words{"Spur"}, Words{"Duct"}, Words{"Walkway"}}, true, {}, {}, "", "scifi"},
        {"mexican",
         {"Las Palmas", "Los Laureles", "Los Mangos", "Las Gaviotas", "del Pescador", "de los Almendros", "Las Jacarandas",
          "del Mar", "de la Ribera", "Los Tabachines", "Las Bugambilias", "del Carrizal", "de las Garzas", "Los Guayabos",
          "del Faro", "Los Pinos", "de la Loma", "del Ancla", "Las Arboledas", "de los Pescadores", "del Muelle", "La Marea",
          "de las Conchas", "Los Cedros", "del Salitre", "de la Ceiba", "Los Sauces", "Las Huertas", "del Arroyo", "Los Naranjos",
          "de la Brisa", "del Caracol", "Los Olivos", "de los Alfareros", "de los Herreros", "del Tule", "Las Amapas",
          "del Venado", "Los Guamúchiles", "de la Higuera", "del Coral", "Las Lomas", "de los Arrieros", "del Molino",
          "Las Golondrinas", "de las Dunas", "El Palmar", "de la Salina", "del Mezquite", "Los Ébanos", "de la Cantera"},
         {Words{"Avenida", "Bulevar", "Calzada"}, Words{"Avenida", "Calle"}, Words{"Calle", "Calle", "Privada"},
          Words{"Callejón", "Andador"}, Words{"Andador", "Sendero", "Escalinata"}},
         true,
         {"Las Palmas", "El Mirador", "Los Laureles", "Lomas del Mar", "Vista Alegre", "El Palmar", "Las Gaviotas", "Los Mangos",
          "La Ribera", "Las Arboledas", "El Encanto", "Los Tabachines", "Loma Linda", "Las Brisas", "El Salitre", "Los Almendros",
          "Las Garzas", "Villa del Sol", "El Carrizal", "Arboledas del Río", "Los Sauces", "La Ceiba", "Campo Alegre",
          "Las Jacarandas", "Los Cedros", "El Faro", "Bugambilias", "Puerta del Mar", "Lomas de la Higuera", "El Mezquital"},
         {},
         "Colonia ",
         "mexican"},
    };
    return c;
}

const Culture& cultureNamed(std::string_view name)
{
    for (const Culture& c : all())
        if (name == c.name)
            return c;
    std::string known;
    for (const Culture& c : all())
        known += (known.empty() ? "" : ", ") + std::string(c.name);
    throw std::runtime_error("unknown naming culture '" + std::string(name) + "' (" + known + ")");
}

const char* pick(const Words& w, uint64_t h) { return w[size_t(h % w.size())]; }

// Place and business vocabularies ------------------------------------------------------------------------------------

/** Pools a pattern draws from: {g} given name or nickname, {s} surname, {e} evocative, {n} nature, {a} saint, {p} a
 * person (given and surname), {#} a number. */
struct Vocab
{
    Words given, surnames, evocative, nature, saints, persons;
    std::map<std::string, Words, std::less<>> programs; // business program -> patterns
    std::map<Kind, Words> kinds;                         // kind -> patterns
    Words business;                                      // any business without a program of its own
};

const Vocab& mexicanVocab()
{
    static const Vocab v = [] {
        Vocab x;
        x.given = {"Lupita", "Chuy", "Toño", "Paco", "Memo", "Lalo", "Nacho", "Beto", "Pepe", "Chema", "Rosy", "Lety", "Chabela",
                   "Juanita", "Tere", "Nena", "Güero", "Flaco", "Chato", "Pancho", "Rigo", "Polo", "Neto", "Tavo", "Cuco",
                   "Lencho", "Rafa", "Moni", "Licha", "Cheli", "Kiko", "Fito", "Meche", "Goyo", "Chayo", "Yoli", "Tito", "Lolis"};
        x.surnames = {"Ibarra", "Valenzuela", "Osuna", "Zazueta", "Salazar", "Robles", "Navarro", "Medina", "Ochoa", "Rentería",
                      "Camacho", "Sandoval", "Urías", "Lizárraga", "Tirado", "Peraza", "Bojórquez", "Gastélum", "Rosales",
                      "Castro", "Lara", "Montes", "Ríos", "Vega", "Soto", "Cázarez", "Millán", "Rubio", "Avendaño", "Domínguez",
                      "Andrade", "Ponce", "Villaseñor", "Quiñónez", "Velarde", "Barraza", "Crespo", "Olivas", "Sepúlveda",
                      "Arámburo", "Loaiza", "Echeverría", "Montoya", "Cota", "Sauceda", "Verdugo", "Amarillas", "Mendívil"};
        x.evocative = {"La Esperanza", "El Paraíso", "La Estrella", "El Faro", "La Gaviota", "El Ancla", "La Palapa", "El Tiburón",
                       "La Sirena", "El Coyote", "La Herradura", "El Gallo de Oro", "La Bonita", "El Rinconcito", "La Güera",
                       "El Compadre", "La Ola", "El Pelícano", "La Marea", "El Caracol", "La Lupita", "El Barco", "La Brisa",
                       "El Delfín", "La Palmera", "El Venado", "La Golondrina", "El Mezquite", "La Escondida", "El Atardecer",
                       "La Fortuna", "El Sombrero", "La Concha", "El Marlín", "La Tortuga", "El Navegante", "La Amistad",
                       "El Arrecife", "La Ballena", "El Nopal", "La Luna Llena", "El Huachinango", "La Calandria", "El Jaguar"};
        x.nature = {"Las Palmas", "Los Pinos", "El Mirador", "Las Garzas", "Los Mangos", "Las Brisas", "El Encanto", "La Loma",
                    "Los Álamos", "Las Huertas", "El Pedregal", "La Ceiba", "El Guamúchil", "Los Sauces", "Las Dunas", "El Palmar",
                    "Los Tabachines", "Las Amapas", "El Salitre", "La Cantera", "Los Mezquites", "El Carrizal", "Las Conchas",
                    "La Higuera", "El Tule", "Los Guayabos", "Las Lomas", "El Cerrito", "La Ribera", "Los Cedros"};
        x.saints = {"San Rafael", "Santa Rosa", "San Judas", "Santa Lucía", "San Isidro", "San Martín", "Santa Teresa", "San Benito",
                    "San Lázaro", "Santa Clara", "San Ramón", "Santa Mónica", "San Pascual", "San Cayetano", "Santa Inés",
                    "San Sebastián", "Santa Bárbara", "San Gabriel", "Santa Elena", "San Bartolo"};
        x.persons = {"Ernesto Lizárraga", "Carmen Valenzuela", "Rodolfo Osuna", "Amalia Zazueta", "Ignacio Peraza", "Rosario Urías",
                     "Abelardo Tirado", "Leonor Gastélum", "Alfonso Rentería", "Herminia Bojórquez", "Eusebio Camacho",
                     "Margarita Robles", "Octavio Medina", "Delfina Ochoa", "Fausto Sandoval", "Elvira Rosales",
                     "Ramiro Echeverría", "Consuelo Arámburo", "Teodoro Loaiza", "Graciela Velarde"};
        auto& p = x.programs;
        p["taqueria"] = {"Taquería {g}", "Tacos {e}", "Taquería {e}", "Tacos {g}"};
        p["restaurant"] = {"Restaurante {e}", "Fonda {g}", "Cocina {g}", "Lonchería {g}", "Comedor {e}"};
        p["seafood"] = {"Mariscos {e}", "Mariscos {g}", "Ostionería {e}", "Cevichería {e}"};
        p["cafe"] = {"Café {e}", "Cafetería {s}", "Café {g}"};
        p["bar"] = {"Bar {e}", "Cantina {e}", "Bar {g}"};
        p["cantina"] = {"Cantina {e}", "Cantina {g}"};
        p["nightclub"] = {"Club {e}", "Disco {e}"};
        p["fastFood"] = {"Antojitos {g}", "Hot Dogs {g}", "Tortas {g}", "Hamburguesas {e}", "Cenaduría {g}"};
        p["bakery"] = {"Panadería {e}", "Panadería {s}", "Pastelería {g}"};
        p["iceCream"] = {"Nevería {e}", "Paletería {e}", "Raspados {g}"};
        p["tortilleria"] = {"Tortillería {e}", "Tortillería {g}"};
        p["pharmacy"] = {"Farmacia {a}", "Farmacia {s}", "Farmacia {e}", "Botica {a}"};
        p["grocery"] = {"Abarrotes {g}", "Abarrotes {e}", "Miscelánea {g}", "Tienda {e}", "Abarrotes {s}"};
        p["supermarket"] = {"Súper {s}", "Supermercado {e}", "Mercadito {e}"};
        p["butcher"] = {"Carnicería {s}", "Carnes {g}", "Carnicería {e}"};
        p["carParts"] = {"Refaccionaria {s}", "Refacciones {e}", "Autopartes {s}"};
        p["tyres"] = {"Llantera {g}", "Vulcanizadora {e}", "Llantas {s}"};
        p["carRepair"] = {"Taller Mecánico {s}", "Taller {g}", "Hojalatería y Pintura {s}", "Mofles {g}"};
        p["carWash"] = {"Autolavado {e}", "Lavado de Autos {g}"};
        p["hardware"] = {"Ferretería {s}", "Materiales {s}", "Ferretería {e}"};
        p["clothes"] = {"Boutique {g}", "Ropa {e}", "Novedades {g}"};
        p["shoes"] = {"Zapatería {s}", "Calzado {e}"};
        p["electronics"] = {"Celulares {e}", "Electrónica {s}", "Reparación de Celulares {g}"};
        p["furniture"] = {"Muebles {s}", "Mueblería {e}"};
        p["stationery"] = {"Papelería {e}", "Papelería {g}"};
        p["hairdresser"] = {"Estética {g}", "Barbería {g}", "Estética {e}"};
        p["laundry"] = {"Lavandería {e}", "Lavandería {g}"};
        p["bank"] = {"Banco {s}", "Financiera {e}", "Caja Popular {a}"};
        p["fuel"] = {"Gasolinera {e}", "Servicio {s}", "Gasolinería {e}"};
        p["hotel"] = {"Hotel {e}", "Hotel {s}", "Posada {e}", "Hotel Playa {e}"};
        p["motel"] = {"Motel {e}", "Auto Hotel {e}"};
        p["mall"] = {"Plaza {e}", "Centro Comercial {n}"};
        p["market"] = {"Mercado {e}", "Mercado Municipal {s}", "Mercado {n}"};
        p["liquor"] = {"Depósito {e}", "Vinos y Licores {s}", "Expendio {g}"};
        p["florist"] = {"Florería {g}", "Florería {e}"};
        p["jewelry"] = {"Joyería {s}"};
        p["optician"] = {"Óptica {s}", "Óptica {e}"};
        p["clinic"] = {"Consultorio Médico {s}", "Clínica {a}", "Clínica {s}"};
        p["dentist"] = {"Consultorio Dental {s}", "Dentista {s}"};
        p["vet"] = {"Veterinaria {e}", "Clínica Veterinaria {s}"};
        p["gym"] = {"Gimnasio {e}", "Gym {g}"};
        p["cinema"] = {"Cine {e}", "Cinema {e}"};
        p["church"] = {"Parroquia de {a}", "Templo de {a}", "Capilla de {a}"};
        p["cathedral"] = {"Catedral de {a}"};
        p["school"] = {"Escuela Primaria {p}", "Secundaria Técnica No. {#}", "Jardín de Niños {e}", "Colegio {a}"};
        p["university"] = {"Universidad {s}", "Instituto Tecnológico {e}", "Instituto {p}"};
        p["hospital"] = {"Hospital {a}", "Hospital General {e}", "Clínica {a}"};
        p["police"] = {"Comandancia de Policía, Sector {#}", "Policía Municipal, Base {#}"};
        p["fireStation"] = {"Estación de Bomberos No. {#}"};
        p["townhall"] = {"Palacio Municipal {e}", "Ayuntamiento {e}"};
        p["government"] = {"Oficina de Gobierno No. {#}", "Junta Municipal {e}", "Recaudación de Rentas No. {#}"};
        p["courthouse"] = {"Juzgado {#}", "Tribunal de {a}"};
        p["military"] = {"Zona Militar {#}", "Cuartel {e}"};
        p["postOffice"] = {"Oficina Postal No. {#}"};
        p["library"] = {"Biblioteca {p}"};
        p["museum"] = {"Museo {e}", "Museo {p}"};
        p["theatre"] = {"Teatro {p}", "Teatro {e}"};
        p["stadium"] = {"Estadio {e}", "Estadio {s}"};
        p["sports"] = {"Unidad Deportiva {s}", "Campo {e}"};
        p["port"] = {"Puerto {e}", "Terminal Marítima {e}"};
        p["ferryTerminal"] = {"Terminal de Transbordadores {e}"};
        p["marina"] = {"Marina {e}"};
        p["airport"] = {"Aeropuerto Internacional {p}", "Aeródromo {e}"};
        p["airstrip"] = {"Pista {e}", "Aeródromo {n}"};
        p["busStation"] = {"Central de Autobuses {e}", "Terminal {e}"};
        p["cemetery"] = {"Panteón {e}", "Panteón Municipal No. {#}", "Cementerio de {a}"};
        p["park"] = {"Parque {e}", "Jardín {a}", "Parque {n}"};
        p["office"] = {"Despacho {s}", "Oficinas {s}"};
        p["warehouse"] = {"Bodega {s}", "Almacenes {e}"};
        p["factory"] = {"Industrias {s}", "Procesadora {e}"};
        p["packingPlant"] = {"Empacadora {e}", "Empacadora {s}"};
        p["coldStorage"] = {"Frigorífico {e}"};
        p["truckYard"] = {"Transportes {s}", "Fletes {g}"};
        p["farm"] = {"Rancho {e}", "Campo Agrícola {e}", "Granja {e}"};
        p["greenhouse"] = {"Invernaderos {e}", "Agrícola {s}"};
        p["lighthouse"] = {"Faro {e}"};
        p["shop"] = {"Tienda {e}", "Comercial {s}", "Novedades {e}"};
        p["building"] = {"Edificio {e}", "Torre {e}", "Condominios {e}"};
        p["apartments"] = {"Condominios {e}", "Departamentos {n}", "Torre {e}"};
        p["mansion"] = {"Quinta {e}", "Villa {e}"};
        p["shipping"] = {"Naviera {e}", "Línea Marítima {s}", "Transportes Marítimos {e}", "Contenedores {s}"};
        p["cruiseLine"] = {"Cruceros {e}", "Cruceros {s}"};
        p["cruiseTerminal"] = {"Terminal de Cruceros {e}", "Muelle de Cruceros {s}"};
        p["customs"] = {"Aduana Marítima {e}", "Aduana {s}"};
        p["mausoleum"] = {"Familia {s} {s}", "Capilla Familiar {s} {s}", "Familia {s}"};
        x.business = {"Comercial {s}", "Tienda {e}", "Negocio {g}"};
        auto& k = x.kinds;
        k[Kind::Town] = {"{a} de {n}", "Villa {s}", "{n}", "Puerto {e}", "Real de {n}"};
        k[Kind::River] = {"Río {n}", "Río {s}", "Río {e}"};
        k[Kind::Stream] = {"Arroyo {e}", "Arroyo {n}"};
        k[Kind::Lake] = {"Laguna {e}", "Laguna {n}"};
        k[Kind::Reservoir] = {"Presa {p}", "Presa {e}"};
        k[Kind::Dam] = {"Cortina {p}", "Presa {p}"};
        k[Kind::Island] = {"Isla {e}", "Isla de {n}"};
        k[Kind::Beach] = {"Playa {e}", "Playa {n}"};
        k[Kind::Bay] = {"Bahía {e}", "Ensenada {e}"};
        k[Kind::Mountain] = {"Cerro {e}", "Cerro {n}"};
        k[Kind::Park] = {"Parque {e}", "Jardín {a}"};
        k[Kind::Landmark] = {"Monumento {e}", "Glorieta {e}", "Mirador {e}"};
        return x;
    }();
    return v;
}

const Vocab& englishVocab()
{
    static const Vocab v = [] {
        Vocab x;
        x.given = {"Joe", "Rosie", "Sal", "Frank", "Dot", "Eddie", "Mo", "Lou", "Betty", "Gus", "Hank", "Ruby", "Stan", "Vera",
                   "Al", "Maggie", "Ned", "Pearl", "Rudy", "Tess", "Walt", "Ivy", "Mick", "Nell", "Otis", "Fern", "Clyde", "June"};
        x.surnames = {"Harlow", "Pemberton", "Whitlock", "Calloway", "Ashby", "Brennan", "Dunmore", "Fairley", "Garrick", "Holloway",
                      "Kessler", "Lindqvist", "Marlowe", "Norcross", "Oakes", "Prescott", "Quimby", "Radley", "Sutter", "Thorne",
                      "Underhill", "Vance", "Winslow", "Yardley", "Abernathy", "Blackwood", "Crowley", "Delancey", "Ellery", "Fenwick"};
        x.evocative = {"Bluebird", "Lucky Star", "Golden Anchor", "Silver Gull", "Red Lantern", "Sunset", "Harbor Light", "Driftwood",
                       "Black Pelican", "Copper Kettle", "Blue Marlin", "Morning Glory", "Old Mill", "Crescent", "Northstar",
                       "Seaside", "Lighthouse", "Sandpiper", "Iron Horse", "Wild Rose", "Tidewater", "Kingfisher", "High Noon",
                       "Lone Pine", "Stardust", "Firefly", "Riverbend", "Cobblestone", "Bayview", "Westwind"};
        x.nature = {"Pine Hollow", "Cedar Ridge", "Willow Creek", "Oak Bluff", "Maple Glen", "Birch Point", "Elm Grove", "Ash Vale",
                    "Fern Hill", "Heron Marsh", "Fox Run", "Stony Brook", "Sandy Hook", "Clear Water", "Eagle Rock", "Deer Lick"};
        x.saints = {"St. Anselm", "St. Brendan", "St. Cuthbert", "St. Dunstan", "St. Edith", "St. Felix", "St. Hilda", "St. Jude",
                    "St. Kevin", "St. Lucy", "St. Martin", "St. Oswald", "St. Agnes", "St. Ambrose"};
        x.persons = {"Edwin Harlow", "Martha Pemberton", "Arthur Whitlock", "Clara Calloway", "Henry Ashby", "Agnes Brennan",
                     "Walter Dunmore", "Edith Fairley", "Silas Garrick", "Ruth Holloway", "Oscar Kessler", "Mabel Lindqvist"};
        auto& p = x.programs;
        p["taqueria"] = {"{g}'s Tacos", "{e} Taco Stand"};
        p["restaurant"] = {"{g}'s Diner", "The {e} Grill", "{e} Kitchen", "{s}'s Restaurant"};
        p["seafood"] = {"{e} Seafood", "{g}'s Fish House", "The {e} Oyster Bar"};
        p["cafe"] = {"{e} Café", "{g}'s Coffee", "The {e} Coffee House"};
        p["bar"] = {"The {e}", "{g}'s Bar", "The {e} Tavern"};
        p["cantina"] = {"The {e} Saloon", "{g}'s Tavern"};
        p["nightclub"] = {"Club {e}", "The {e} Lounge"};
        p["fastFood"] = {"{g}'s Burgers", "{e} Drive-In", "{g}'s Hot Dogs"};
        p["bakery"] = {"{s} Bakery", "{e} Bakery", "{g}'s Bake Shop"};
        p["iceCream"] = {"{e} Creamery", "{g}'s Ice Cream"};
        p["tortilleria"] = {"{e} Tortilla Factory"};
        p["pharmacy"] = {"{s} Pharmacy", "{e} Drugs", "{s} Apothecary"};
        p["grocery"] = {"{g}'s Corner Store", "{e} Market", "{s} Grocery"};
        p["supermarket"] = {"{s} Foods", "{e} Supermarket"};
        p["butcher"] = {"{s} Butchers", "{g}'s Meats"};
        p["carParts"] = {"{s} Auto Parts", "{e} Parts Supply"};
        p["tyres"] = {"{g}'s Tire Shop", "{s} Tire & Wheel"};
        p["carRepair"] = {"{s} Motors", "{g}'s Garage", "{e} Auto Repair"};
        p["carWash"] = {"{e} Car Wash"};
        p["hardware"] = {"{s} Hardware", "{e} Lumber & Supply"};
        p["clothes"] = {"{e} Outfitters", "{g}'s Boutique"};
        p["shoes"] = {"{s} Shoes"};
        p["electronics"] = {"{e} Electronics", "{s} Radio & TV"};
        p["furniture"] = {"{s} Furniture", "{e} Home"};
        p["stationery"] = {"{e} Stationers"};
        p["hairdresser"] = {"{g}'s Barbershop", "{e} Salon"};
        p["laundry"] = {"{e} Laundromat"};
        p["bank"] = {"{s} Savings & Loan", "{e} Trust", "{s} Bank"};
        p["fuel"] = {"{e} Gas", "{s} Service Station"};
        p["hotel"] = {"Hotel {e}", "The {s} Hotel", "{e} Inn"};
        p["motel"] = {"{e} Motel", "{e} Motor Lodge"};
        p["mall"] = {"{e} Mall", "{e} Plaza"};
        p["market"] = {"{e} Market", "{s} Market Hall"};
        p["liquor"] = {"{e} Liquors", "{g}'s Bottle Shop"};
        p["florist"] = {"{g}'s Flowers"};
        p["jewelry"] = {"{s} Jewelers"};
        p["optician"] = {"{s} Optical"};
        p["clinic"] = {"{s} Medical Center", "{a} Clinic"};
        p["dentist"] = {"{s} Dental"};
        p["vet"] = {"{e} Animal Hospital"};
        p["gym"] = {"{e} Fitness", "{g}'s Gym"};
        p["cinema"] = {"{e} Theater", "The {e} Cinema"};
        p["church"] = {"{a} Church", "{a} Chapel"};
        p["cathedral"] = {"Cathedral of {a}"};
        p["school"] = {"{p} Elementary", "{nature} Middle School", "{s} High School"};
        p["university"] = {"{s} University", "{e} College"};
        p["hospital"] = {"{a} Hospital", "{s} General Hospital"};
        p["police"] = {"Police Precinct {#}", "Police Station {#}"};
        p["fireStation"] = {"Fire Station {#}"};
        p["townhall"] = {"City Hall", "{e} Town Hall"};
        p["government"] = {"Government Office {#}", "{s} Municipal Building"};
        p["courthouse"] = {"{s} Courthouse"};
        p["military"] = {"Fort {s}", "Camp {s}"};
        p["postOffice"] = {"Post Office {#}"};
        p["library"] = {"{p} Library"};
        p["museum"] = {"{e} Museum", "{p} Museum"};
        p["theatre"] = {"The {e} Playhouse", "{s} Theatre"};
        p["stadium"] = {"{e} Stadium", "{s} Field"};
        p["sports"] = {"{s} Sports Ground", "{e} Park"};
        p["port"] = {"Port {s}", "{e} Docks"};
        p["ferryTerminal"] = {"{e} Ferry Terminal"};
        p["marina"] = {"{e} Marina"};
        p["airport"] = {"{p} International Airport", "{s} Field"};
        p["airstrip"] = {"{s} Airstrip"};
        p["busStation"] = {"{e} Bus Depot"};
        p["cemetery"] = {"{e} Cemetery", "{a} Cemetery"};
        p["park"] = {"{e} Park", "{s} Park"};
        p["office"] = {"{s} & {s} Offices", "{s} Building"};
        p["warehouse"] = {"{s} Storage", "{e} Warehouse"};
        p["factory"] = {"{s} Industries", "{e} Works"};
        p["packingPlant"] = {"{e} Packing Co.", "{s} Packers"};
        p["coldStorage"] = {"{e} Cold Storage"};
        p["truckYard"] = {"{s} Trucking", "{e} Freight"};
        p["farm"] = {"{e} Farm", "{s} Ranch"};
        p["greenhouse"] = {"{e} Greenhouses", "{s} Growers"};
        p["lighthouse"] = {"{e} Lighthouse"};
        p["shop"] = {"{e} Store", "{s} & Sons"};
        p["building"] = {"The {e} Building", "{s} Tower"};
        p["apartments"] = {"{e} Apartments", "{s} Court"};
        p["mansion"] = {"{s} House", "{e} Manor"};
        p["shipping"] = {"{e} Lines", "{s} Shipping", "{e} Container Line"};
        p["cruiseLine"] = {"{e} Cruises", "{s} Cruise Line"};
        p["cruiseTerminal"] = {"{e} Cruise Terminal", "{s} Pier"};
        p["customs"] = {"{e} Customs House", "{s} Customs Post"};
        p["mausoleum"] = {"{s} Family", "{s}-{s} Family", "{s} Family Chapel"};
        x.business = {"{s} & Co.", "{e} Store", "{g}'s"};
        auto& k = x.kinds;
        k[Kind::Town] = {"{n}", "Port {s}", "{s}ville", "{s} Springs", "New {s}"};
        k[Kind::River] = {"{s} River", "{e} River"};
        k[Kind::Stream] = {"{e} Creek", "{s} Brook"};
        k[Kind::Lake] = {"Lake {s}", "{e} Lake"};
        k[Kind::Reservoir] = {"{s} Reservoir"};
        k[Kind::Dam] = {"{s} Dam"};
        k[Kind::Island] = {"{s} Island", "{e} Island"};
        k[Kind::Beach] = {"{e} Beach", "{s} Beach"};
        k[Kind::Bay] = {"{s} Bay", "{e} Cove"};
        k[Kind::Mountain] = {"Mount {s}", "{e} Peak"};
        k[Kind::Park] = {"{e} Park", "{s} Park"};
        k[Kind::Landmark] = {"{e} Monument", "{s} Memorial"};
        return x;
    }();
    return v;
}

const Vocab& frenchVocab()

{

    static const Vocab v = [] {

        Vocab x;

        x.given = {"Jojo", "Mimi", "Dédé", "Lulu", "Fanfan", "Riri", "Gégé", "Titi", "Nono", "Loulou", "Pépé", "Mémé", "Bébert",

                   "Momo", "Coco", "Zaza", "Fifi", "Toto", "Néné", "Bibi", "Marcel", "Odette", "Gaston", "Simone", "Lucien", "Yvette"};

        x.surnames = {"Dubreuil", "Lemaire", "Vasseur", "Delacroix", "Marchand", "Fontaine", "Rochefort", "Bonnard", "Duclos",

                      "Lefèvre", "Chaumont", "Garnier", "Morel", "Brissac", "Lambert", "Roussel", "Cordier", "Daumier", "Vernet",

                      "Perrin", "Aubert", "Clément", "Mercier", "Thibault", "Legrand", "Masson", "Boucher", "Charpentier"};

        x.evocative = {"Le Bon Accueil", "La Belle Époque", "La Petite Barque", "L'Étoile d'Or", "Le Coq Hardi", "La Mouette",

                       "Le Vieux Port", "La Lanterne", "Le Lion d'Argent", "La Rose des Vents", "Le Phare", "La Sirène", "Le Cheval Pie",

                       "La Bonne Mère", "Le Moulin Bleu", "Les Trois Pigeons", "Le Grand Cerf", "La Fleur de Lys", "Le Soleil Levant",

                       "La Belle Meunière", "L'Ancre", "Le Chat Gris", "La Licorne", "Le Pélican", "La Gaieté", "Le Rivage"};

        x.nature = {"Les Pins", "Les Tilleuls", "Bois-Joli", "Les Saules", "Val Fleuri", "Les Chênes", "La Garenne", "Les Mimosas",

                    "Beauséjour", "Les Peupliers", "Mont-Fleuri", "La Pinède", "Les Glycines", "Bellevue", "Les Charmes", "Clairefontaine"};

        x.saints = {"Saint-Aubin", "Sainte-Anne", "Saint-Éloi", "Sainte-Claire", "Saint-Florent", "Sainte-Marthe", "Saint-Gildas",

                    "Sainte-Odile", "Saint-Hilaire", "Sainte-Agathe", "Saint-Rémi", "Sainte-Cécile", "Saint-Yves", "Saint-Blaise"};

        x.persons = {"Émile Dubreuil", "Jeanne Lemaire", "Aristide Vasseur", "Hortense Delacroix", "Achille Marchand",

                     "Berthe Fontaine", "Honoré Rochefort", "Léonie Bonnard", "Firmin Duclos", "Adèle Lefèvre", "Anatole Chaumont",

                     "Pauline Garnier"};

        auto& p = x.programs;

        p["taqueria"] = {"Tacos {g}", "Comptoir à Tacos {e}"};

        p["restaurant"] = {"Restaurant {e}", "Chez {g}", "Bistrot {s}", "Brasserie {e}", "Auberge {e}"};

        p["seafood"] = {"Poissonnerie {s}", "La Marée de {g}", "Huîtres {e}"};

        p["cafe"] = {"Café {e}", "Café des {s}", "Salon de Thé {g}"};

        p["bar"] = {"Bar {e}", "Le Bar de {g}", "Estaminet {e}"};

        p["cantina"] = {"Taverne {e}", "Buvette {g}"};

        p["nightclub"] = {"Club {e}", "Cabaret {e}"};

        p["fastFood"] = {"Crêperie {g}", "Sandwicherie {e}", "Friterie {g}"};

        p["bakery"] = {"Boulangerie {s}", "Boulangerie {e}", "Pâtisserie {g}"};

        p["iceCream"] = {"Glacier {e}", "Glaces {g}"};

        p["tortilleria"] = {"Tortillas {e}"};

        p["pharmacy"] = {"Pharmacie {a}", "Pharmacie {s}", "Pharmacie du {e}"};

        p["grocery"] = {"Épicerie {g}", "Épicerie {e}", "Alimentation {s}"};

        p["supermarket"] = {"Marché {s}", "Supérette {e}"};

        p["butcher"] = {"Boucherie {s}", "Charcuterie {g}"};

        p["carParts"] = {"Pièces Auto {s}", "Accessoires {e}"};

        p["tyres"] = {"Pneus {s}", "Pneumatiques {g}"};

        p["carRepair"] = {"Garage {s}", "Garage {e}", "Carrosserie {g}"};

        p["carWash"] = {"Lavage {e}"};

        p["hardware"] = {"Quincaillerie {s}", "Droguerie {e}"};

        p["clothes"] = {"Boutique {g}", "Mode {e}", "Maison {s}"};

        p["shoes"] = {"Chaussures {s}"};

        p["electronics"] = {"Radio {s}", "Électronique {e}"};

        p["furniture"] = {"Meubles {s}", "Ébénisterie {e}"};

        p["stationery"] = {"Papeterie {e}", "Librairie {s}"};

        p["hairdresser"] = {"Salon {g}", "Coiffure {e}", "Barbier {g}"};

        p["laundry"] = {"Laverie {e}", "Blanchisserie {s}"};

        p["bank"] = {"Banque {s}", "Crédit {e}", "Caisse {s}"};

        p["fuel"] = {"Station {e}", "Relais {s}"};

        p["hotel"] = {"Hôtel {e}", "Hôtel {s}", "Grand Hôtel du {e}"};

        p["motel"] = {"Relais {e}", "Motel {e}"};

        p["mall"] = {"Galerie {e}", "Passage {s}"};

        p["market"] = {"Marché {e}", "Halles {s}"};

        p["liquor"] = {"Cave {s}", "Vins {g}"};

        p["florist"] = {"Fleurs {g}", "Fleuriste {e}"};

        p["jewelry"] = {"Bijouterie {s}", "Joaillerie {e}"};

        p["optician"] = {"Optique {s}"};

        p["clinic"] = {"Clinique {a}", "Cabinet Médical {s}"};

        p["dentist"] = {"Cabinet Dentaire {s}"};

        p["vet"] = {"Clinique Vétérinaire {e}"};

        p["gym"] = {"Gymnase {e}", "Salle {s}"};

        p["cinema"] = {"Cinéma {e}", "Ciné {s}"};

        p["church"] = {"Église {a}", "Chapelle {a}"};

        p["cathedral"] = {"Cathédrale {a}"};

        p["school"] = {"École {p}", "Collège {p}", "Lycée {p}"};

        p["university"] = {"Université {s}", "Faculté {e}"};

        p["hospital"] = {"Hôpital {a}", "Centre Hospitalier {s}"};

        p["police"] = {"Commissariat {#}", "Gendarmerie {s}"};

        p["fireStation"] = {"Caserne des Pompiers {#}"};

        p["townhall"] = {"Hôtel de Ville", "Mairie {e}"};

        p["government"] = {"Préfecture {s}", "Bureau Municipal {#}"};

        p["courthouse"] = {"Palais de Justice {s}"};

        p["military"] = {"Caserne {s}", "Fort {s}"};

        p["postOffice"] = {"Bureau de Poste {#}"};

        p["library"] = {"Bibliothèque {p}"};

        p["museum"] = {"Musée {p}", "Musée {e}"};

        p["theatre"] = {"Théâtre {e}", "Théâtre {s}"};

        p["stadium"] = {"Stade {p}", "Stade {e}"};

        p["sports"] = {"Terrain {s}", "Complexe Sportif {e}"};

        p["port"] = {"Port {s}", "Quais {e}"};

        p["ferryTerminal"] = {"Gare Maritime {e}"};

        p["marina"] = {"Port de Plaisance {e}"};

        p["airport"] = {"Aéroport {p}", "Aérodrome {s}"};

        p["airstrip"] = {"Piste {s}"};

        p["busStation"] = {"Gare Routière {e}"};

        p["cemetery"] = {"Cimetière {a}", "Cimetière {e}"};

        p["park"] = {"Jardin {e}", "Parc {s}"};

        p["office"] = {"Bureaux {s}", "Immeuble {s}"};

        p["warehouse"] = {"Entrepôts {s}", "Dépôt {e}"};

        p["factory"] = {"Usine {s}", "Manufacture {e}"};

        p["packingPlant"] = {"Conserverie {e}", "Coopérative {s}"};

        p["coldStorage"] = {"Entrepôt Frigorifique {e}"};

        p["truckYard"] = {"Transports {s}", "Routiers {e}"};

        p["farm"] = {"Ferme {e}", "Domaine {s}", "Mas {s}"};

        p["greenhouse"] = {"Serres {e}", "Horticulture {s}"};

        p["lighthouse"] = {"Phare {e}"};

        p["shop"] = {"Maison {s}", "Comptoir {e}"};

        p["building"] = {"Immeuble {e}", "Résidence {s}"};

        p["apartments"] = {"Résidence {e}", "Les Jardins de {n}"};

        p["mansion"] = {"Hôtel Particulier {s}", "Château {s}"};

        x.business = {"Maison {s}", "Comptoir {e}", "Chez {g}"};

        auto& k = x.kinds;

        k[Kind::Town] = {"{a}", "{a}-sur-Mer", "Villeneuve-{s}", "{n}", "Port-{s}"};

        k[Kind::River] = {"La {s}", "Rivière {e}"};

        k[Kind::Stream] = {"Ruisseau {e}", "Ru de {n}"};

        k[Kind::Lake] = {"Lac {s}", "Étang {e}"};

        k[Kind::Reservoir] = {"Lac de Retenue {s}"};

        k[Kind::Dam] = {"Barrage {s}"};

        k[Kind::Island] = {"Île {e}", "Île {s}"};

        k[Kind::Beach] = {"Plage {e}", "Plage des {n}"};

        k[Kind::Bay] = {"Baie {s}", "Anse {e}"};

        k[Kind::Mountain] = {"Mont {s}", "Pic {e}"};

        k[Kind::Park] = {"Jardin {e}", "Parc {s}"};

        k[Kind::Landmark] = {"Monument {e}", "Colonne {s}"};

        return x;

    }();

    return v;

}



const Vocab& latinVocab()

{

    static const Vocab v = [] {

        Vocab x;

        x.given = {"Marcus", "Lucius", "Gaius", "Quintus", "Titus", "Aulus", "Decimus", "Sextus", "Publius", "Gnaeus", "Servius",

                   "Tiberius", "Julia", "Livia", "Octavia", "Cornelia", "Claudia", "Flavia", "Aurelia", "Valeria", "Tullia", "Fabia"};

        x.surnames = {"Varro", "Cato", "Rufus", "Crispus", "Naso", "Strabo", "Lentulus", "Calvus", "Balbus", "Celer", "Longinus",

                      "Paetus", "Severus", "Felix", "Pulcher", "Bassus", "Macer", "Niger", "Priscus", "Vetus", "Sabinus", "Galba"};

        x.evocative = {"Aquila", "Lupa", "Delphinus", "Columba", "Fortuna", "Concordia", "Victoria", "Pax", "Spes", "Aurora",

                       "Sol Invictus", "Luna", "Stella Maris", "Ancora", "Leo Aureus", "Ceres", "Neptunus", "Minerva", "Bacchus",

                       "Mercurius", "Vulcanus", "Faunus", "Pomona", "Flora"};

        x.nature = {"Olivetum", "Pinetum", "Laurentum", "Fons Clarus", "Saxa Rubra", "Vallis Amoena", "Silva Nigra", "Collis Viridis",

                    "Ripa Alta", "Campus Florum", "Lacus Placidus", "Mons Albus", "Quercetum", "Vinea Nova"};

        x.saints = {"Iovis", "Iunonis", "Martis", "Apollinis", "Dianae", "Vestae", "Saturni", "Herculis", "Castoris", "Ianus",

                    "Veneris", "Fortunae", "Mercurii", "Cereris"};

        x.persons = {"Marcus Varro", "Lucius Crispus", "Gaius Severus", "Quintus Balbus", "Titus Longinus", "Aulus Priscus",

                     "Publius Sabinus", "Decimus Celer", "Julia Felix", "Cornelia Pulchra", "Livia Macrina", "Claudia Severa"};

        auto& p = x.programs;

        p["taqueria"] = {"Popina {g}", "Thermopolium {e}"};

        p["restaurant"] = {"Popina {e}", "Caupona {g}", "Taberna {s}"};

        p["seafood"] = {"Piscaria {s}", "Ostrearia {e}"};

        p["cafe"] = {"Thermopolium {e}", "Taberna {g}"};

        p["bar"] = {"Caupona {e}", "Taberna Vinaria {g}"};

        p["cantina"] = {"Caupona {g}"};

        p["nightclub"] = {"Lupanar {e}", "Taberna {e}"};

        p["fastFood"] = {"Thermopolium {g}", "Popina {e}"};

        p["bakery"] = {"Pistrinum {s}", "Pistrinum {e}"};

        p["iceCream"] = {"Nivaria {e}"};

        p["tortilleria"] = {"Pistrinum {g}"};

        p["pharmacy"] = {"Medicamentaria {s}", "Taberna Medica {e}"};

        p["grocery"] = {"Taberna {g}", "Macellum {e}"};

        p["supermarket"] = {"Macellum {s}", "Horrea {e}"};

        p["butcher"] = {"Laniena {s}", "Lanius {g}"};

        p["carParts"] = {"Fabrica Rotarum {s}"};

        p["tyres"] = {"Rotaria {g}"};

        p["carRepair"] = {"Fabrica Vehiculorum {s}", "Officina {g}"};

        p["carWash"] = {"Lavatrina {e}"};

        p["hardware"] = {"Ferraria {s}", "Officina Fabri {e}"};

        p["clothes"] = {"Vestiaria {g}", "Fullonica {e}"};

        p["shoes"] = {"Sutrina {s}"};

        p["electronics"] = {"Officina {e}"};

        p["furniture"] = {"Lignaria {s}"};

        p["stationery"] = {"Libraria {e}", "Chartaria {s}"};

        p["hairdresser"] = {"Tonstrina {g}", "Tonstrina {e}"};

        p["laundry"] = {"Fullonica {s}"};

        p["bank"] = {"Argentaria {s}", "Mensa {e}"};

        p["fuel"] = {"Statio {e}", "Mansio {s}"};

        p["hotel"] = {"Hospitium {e}", "Mansio {s}"};

        p["motel"] = {"Mansio {e}", "Stabulum {g}"};

        p["mall"] = {"Porticus {e}", "Basilica {s}"};

        p["market"] = {"Macellum {e}", "Forum {s}"};

        p["liquor"] = {"Vinaria {s}", "Taberna Vinaria {e}"};

        p["florist"] = {"Coronaria {g}"};

        p["jewelry"] = {"Aurifex {s}", "Gemmaria {e}"};

        p["optician"] = {"Ocularia {s}"};

        p["clinic"] = {"Valetudinarium {e}", "Medicus {s}"};

        p["dentist"] = {"Medicus Dentium {s}"};

        p["vet"] = {"Mulomedicus {s}"};

        p["gym"] = {"Palaestra {e}", "Gymnasium {s}"};

        p["cinema"] = {"Odeum {e}"};

        p["church"] = {"Aedes {a}", "Sacellum {a}"};

        p["cathedral"] = {"Templum {a}"};

        p["school"] = {"Ludus {p}", "Schola {s}"};

        p["university"] = {"Athenaeum {s}", "Academia {e}"};

        p["hospital"] = {"Valetudinarium {a}"};

        p["police"] = {"Statio Vigilum {#}", "Excubitorium {#}"};

        p["fireStation"] = {"Statio Vigilum {#}"};

        p["townhall"] = {"Curia", "Curia {e}"};

        p["government"] = {"Tabularium {s}", "Praetorium {#}"};

        p["courthouse"] = {"Basilica Iudicaria {s}"};

        p["military"] = {"Castra {s}", "Castellum {e}"};

        p["postOffice"] = {"Cursus Publicus {#}"};

        p["library"] = {"Bibliotheca {p}"};

        p["museum"] = {"Museum {e}", "Pinacotheca {p}"};

        p["theatre"] = {"Theatrum {s}", "Theatrum {e}"};

        p["stadium"] = {"Amphitheatrum {s}", "Circus {e}"};

        p["sports"] = {"Campus {s}", "Palaestra {e}"};

        p["port"] = {"Portus {s}", "Navalia {e}"};

        p["ferryTerminal"] = {"Statio Navium {e}"};

        p["marina"] = {"Portus {e}"};

        p["airport"] = {"Aeroportus {p}", "Campus Volatus {s}"};

        p["airstrip"] = {"Campus Volatus {s}"};

        p["busStation"] = {"Statio Raedarum {e}"};

        p["cemetery"] = {"Necropolis {e}", "Sepulcretum {a}"};

        p["park"] = {"Horti {s}", "Lucus {e}"};

        p["office"] = {"Tabularium {s}", "Insula {s}"};

        p["warehouse"] = {"Horrea {s}", "Apotheca {e}"};

        p["factory"] = {"Officina {s}", "Fabrica {e}"};

        p["packingPlant"] = {"Salsamentaria {e}", "Officina {s}"};

        p["coldStorage"] = {"Cella Nivaria {e}"};

        p["truckYard"] = {"Stabulum Plaustrorum {s}"};

        p["farm"] = {"Villa Rustica {s}", "Fundus {e}"};

        p["greenhouse"] = {"Hortus {e}"};

        p["lighthouse"] = {"Pharus {e}"};

        p["shop"] = {"Taberna {s}", "Officina {e}"};

        p["building"] = {"Insula {e}", "Domus {s}"};

        p["apartments"] = {"Insula {e}", "Insula {s}"};

        p["mansion"] = {"Domus {s}", "Villa {s}"};

        x.business = {"Taberna {s}", "Officina {e}", "Taberna {g}"};

        auto& k = x.kinds;

        k[Kind::Town] = {"Colonia {s}", "Forum {s}", "{n}", "Portus {e}", "Castra {s}"};

        k[Kind::River] = {"Flumen {s}", "Flumen {e}"};

        k[Kind::Stream] = {"Rivus {e}", "Rivus {s}"};

        k[Kind::Lake] = {"Lacus {s}", "Lacus {e}"};

        k[Kind::Reservoir] = {"Piscina {s}"};

        k[Kind::Dam] = {"Moles {s}"};

        k[Kind::Island] = {"Insula {e}", "Insula {s}"};

        k[Kind::Beach] = {"Litus {e}", "Litus {s}"};

        k[Kind::Bay] = {"Sinus {s}", "Sinus {e}"};

        k[Kind::Mountain] = {"Mons {s}", "Mons {e}"};

        k[Kind::Park] = {"Horti {s}", "Lucus {e}"};

        k[Kind::Landmark] = {"Arcus {s}", "Columna {e}"};

        return x;

    }();

    return v;

}



const Vocab& scifiVocab()

{

    static const Vocab v = [] {

        Vocab x;

        x.given = {"Zed", "Kira", "Vex", "Nova", "Rho", "Juno", "Tycho", "Lyra", "Orin", "Sable", "Kade", "Echo", "Pax", "Ryn",

                   "Quill", "Vega", "Dax", "Mira", "Cobalt", "Jett"};

        x.surnames = {"Okonkwo-Hale", "Vashti", "Kepler", "Ansari", "Takeda", "Novak", "Halloran", "Ishikawa", "Mbeki", "Sorensen",

                      "Castellan", "Varga", "Oyelaran", "Petrov", "Lindgren", "Achebe", "Moreau", "Tanaka", "Reyes", "Volkov"};

        x.evocative = {"Helix", "Quasar", "Spindrift", "Vector", "Nebula", "Ion", "Apogee", "Aurora", "Photon", "Meridian", "Orbit",

                       "Pulsar", "Halcyon", "Axiom", "Cinder", "Arc", "Lumen", "Flux", "Bulwark", "Solstice", "Parallax", "Nadir"};

        x.nature = {"Hydroponics", "Biodome", "Arboretum", "Greenring", "Lichen Deck", "Algae Bay", "Fern Vault", "Moss Terrace",

                    "Seedbank", "Rain Hall"};

        x.saints = {"Sol", "Terra", "Luna", "Ceres", "Titan", "Europa", "Io", "Vesta", "Callisto", "Triton"};

        x.persons = {"Ada Okonkwo-Hale", "Teo Vashti", "Mira Kepler", "Idris Ansari", "Ren Takeda", "Lena Novak", "Cal Halloran",

                     "Yuki Ishikawa", "Sipho Mbeki", "Ingrid Sorensen"};

        auto& p = x.programs;

        p["taqueria"] = {"Taco Unit {#}", "{g}'s Wraps"};

        p["restaurant"] = {"Mess Hall {#}", "{e} Galley", "{g}'s Canteen"};

        p["seafood"] = {"{e} Aquafarm Bar", "Kelp & Krill {#}"};

        p["cafe"] = {"{e} Caf", "Stim Bar {#}", "{g}'s Brew"};

        p["bar"] = {"The {e} Lounge", "Bar {#}", "{g}'s Den"};

        p["cantina"] = {"{e} Cantina", "{g}'s Hole"};

        p["nightclub"] = {"Club {e}", "{e} Pulse"};

        p["fastFood"] = {"Ration Stall {#}", "{g}'s Noodles", "{e} Bites"};

        p["bakery"] = {"Protein Bakery {#}", "{e} Ovens"};

        p["iceCream"] = {"Cryo Treats {#}", "{e} Freeze"};

        p["tortilleria"] = {"Flatbread Press {#}"};

        p["pharmacy"] = {"Medbay Dispensary {#}", "{e} Pharma"};

        p["grocery"] = {"Supply Depot {#}", "{g}'s Provisions"};

        p["supermarket"] = {"{e} Commissary", "Commissary {#}"};

        p["butcher"] = {"Vat Meats {#}", "{s} Proteins"};

        p["carParts"] = {"{s} Drive Parts", "Spares Bay {#}"};

        p["tyres"] = {"Track & Wheel {#}"};

        p["carRepair"] = {"{s} Rover Works", "Hangar {#} Repairs", "{g}'s Mech Shop"};

        p["carWash"] = {"Decon Bay {#}"};

        p["hardware"] = {"{s} Fabrication", "Parts Locker {#}"};

        p["clothes"] = {"{e} Outfitters", "Suit Shop {#}"};

        p["shoes"] = {"Mag Boots {#}"};

        p["electronics"] = {"{e} Circuits", "{s} Systems", "Comms Shop {#}"};

        p["furniture"] = {"{e} Habitat Goods"};

        p["stationery"] = {"Datapad Supply {#}"};

        p["hairdresser"] = {"{g}'s Cuts", "{e} Grooming"};

        p["laundry"] = {"Sonic Wash {#}"};

        p["bank"] = {"{e} Credit Union", "{s} Exchange", "Credit Hub {#}"};

        p["fuel"] = {"Fuel Cell Station {#}", "{e} Charge"};

        p["hotel"] = {"{e} Hostel", "Capsule Hotel {#}", "The {e} Suites"};

        p["motel"] = {"Transit Pods {#}"};

        p["mall"] = {"{e} Concourse", "Promenade {#}"};

        p["market"] = {"{e} Bazaar", "Trade Deck {#}"};

        p["liquor"] = {"Synth Spirits {#}", "{g}'s Still"};

        p["florist"] = {"{e} Botanics"};

        p["jewelry"] = {"{e} Crystal Works"};

        p["optician"] = {"Optics Lab {#}"};

        p["clinic"] = {"Medbay {#}", "{s} Clinic"};

        p["dentist"] = {"Dental Bay {#}"};

        p["vet"] = {"Xenovet {#}"};

        p["gym"] = {"Gravity Gym {#}", "{e} Conditioning"};

        p["cinema"] = {"{e} Holotheatre"};

        p["church"] = {"Chapel of {a}", "Shrine of {a}"};

        p["cathedral"] = {"Sanctum of {a}"};

        p["school"] = {"Learning Center {#}", "{p} Academy"};

        p["university"] = {"{s} Institute", "{e} Polytechnic"};

        p["hospital"] = {"Med Center {#}", "{s} Medical"};

        p["police"] = {"Security Post {#}", "Enforcement Station {#}"};

        p["fireStation"] = {"Fire Control {#}"};

        p["townhall"] = {"Administration", "{e} Council Hall"};

        p["government"] = {"Admin Block {#}", "{s} Directorate"};

        p["courthouse"] = {"Tribunal {#}"};

        p["military"] = {"Garrison {s}", "Outpost {#}"};

        p["postOffice"] = {"Mail Node {#}"};

        p["library"] = {"{p} Archive"};

        p["museum"] = {"{e} Heritage Vault", "{p} Museum"};

        p["theatre"] = {"{e} Stage"};

        p["stadium"] = {"{e} Arena", "Arena {#}"};

        p["sports"] = {"Rec Deck {#}"};

        p["port"] = {"Dock {#}", "{e} Docks"};

        p["ferryTerminal"] = {"Shuttle Terminal {#}"};

        p["marina"] = {"Skiff Berths {#}"};

        p["airport"] = {"{p} Spaceport", "Launch Field {#}"};

        p["airstrip"] = {"Landing Pad {#}"};

        p["busStation"] = {"Transit Hub {#}"};

        p["cemetery"] = {"{e} Memorial Columbarium", "Rest Ring {#}"};

        p["park"] = {"{n}", "{e} Commons"};

        p["office"] = {"{s} Offices", "Office Block {#}"};

        p["warehouse"] = {"Storage Bay {#}", "{s} Logistics"};

        p["factory"] = {"{s} Fabricators", "Fab Plant {#}"};

        p["packingPlant"] = {"Packing Line {#}"};

        p["coldStorage"] = {"Cryo Store {#}"};

        p["truckYard"] = {"Hauler Yard {#}", "{s} Freight"};

        p["farm"] = {"{n} {#}", "Agri Ring {#}"};

        p["greenhouse"] = {"{n} Grow Bay"};

        p["lighthouse"] = {"Beacon {#}"};

        p["shop"] = {"{s} Supply", "{e} Goods"};

        p["building"] = {"Block {#}", "{e} Tower"};

        p["apartments"] = {"Hab Block {#}", "{e} Residences"};

        p["mansion"] = {"{s} Estate", "{e} Penthouse"};

        x.business = {"{s} Supply", "{e} Goods", "{g}'s"};

        auto& k = x.kinds;

        k[Kind::Town] = {"{a} Station", "New {a}", "{e} Colony", "Outpost {e}", "{a} Prime"};

        k[Kind::River] = {"{e} Channel", "Conduit {#}"};

        k[Kind::Stream] = {"Runoff {#}", "{e} Rill"};

        k[Kind::Lake] = {"{e} Basin", "Reservoir {#}"};

        k[Kind::Reservoir] = {"Water Store {#}"};

        k[Kind::Dam] = {"{e} Barrier"};

        k[Kind::Island] = {"{e} Atoll", "Platform {#}"};

        k[Kind::Beach] = {"{e} Shore"};

        k[Kind::Bay] = {"{e} Bay"};

        k[Kind::Mountain] = {"{e} Spire", "Peak {#}"};

        k[Kind::Park] = {"{n}", "{e} Commons"};

        k[Kind::Landmark] = {"{e} Monolith", "{e} Obelisk"};

        return x;

    }();

    return v;

}



const Vocab& vocab(std::string_view culture)
{
    const std::string name = vocabularyOf(culture);
    return name == "mexican" ? mexicanVocab() : name == "french" ? frenchVocab() : name == "latin" ? latinVocab() : name == "scifi" ? scifiVocab() : englishVocab();
}

/** The program a kind names its things as when it has no patterns of its own. */
const char* kindProgram(Kind k)
{
    switch (k)
    {
    case Kind::Port: return "port";
    case Kind::Airport: return "airport";
    case Kind::Cemetery: return "cemetery";
    case Kind::School: return "school";
    case Kind::Hospital: return "hospital";
    case Kind::Government: return "government";
    case Kind::Police: return "police";
    case Kind::Religious: return "church";
    case Kind::Stadium: return "stadium";
    case Kind::Market: return "market";
    case Kind::Building: return "building";
    default: return "";
    }
}

std::string expand(const Vocab& v, std::string_view pattern, uint64_t k)
{
    std::string out;
    int slot = 0;
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] != '{')
        {
            out += pattern[i];
            continue;
        }
        const size_t end = pattern.find('}', i);
        const std::string_view tag = pattern.substr(i + 1, end - i - 1);
        const uint64_t h = hash64(k ^ (0xA24BAED4963EE407ull * uint64_t(++slot)));
        if (tag == "g")
            out += pick(v.given, h);
        else if (tag == "s")
            out += pick(v.surnames, h);
        else if (tag == "e")
            out += pick(v.evocative, h);
        else if (tag == "n" || tag == "nature")
            out += pick(v.nature, h);
        else if (tag == "a")
            out += pick(v.saints, h);
        else if (tag == "p")
            out += pick(v.persons, h);
        else if (tag == "#")
            out += std::to_string(1 + int(h % 48));
        i = end;
    }
    // "San San Rafael de ..." and "El El Faro": a pattern's own article or title meets the word's.
    for (const char* dup : {"San San ", "San Santa ", "El El ", "El La ", "El Las ", "El Los "})
        if (out.starts_with(dup))
            out.erase(0, std::string_view(dup).find(' ') + 1);
    return out;
}
} // namespace

std::vector<std::string> cultures()
{
    std::vector<std::string> out;
    for (const Culture& c : all())
        out.emplace_back(c.name);
    return out;
}

bool hasCulture(std::string_view culture)
{
    return std::ranges::any_of(all(), [&](const Culture& c) { return culture == c.name; });
}

void requireCulture(std::string_view culture) { cultureNamed(culture); }

std::string cultureLabel(std::string_view culture)
{
    static const std::pair<const char*, const char*> kLabels[] = {{"oldEnglish", "Old English"}, {"latin", "Latin"}, {"french", "French"},
                                                                 {"spanish", "Spanish"},        {"usGrid", "US Grid"}, {"sector", "Sector Codes"},
                                                                 {"mexican", "Mexican"}};
    for (const auto& [k, v] : kLabels)
        if (culture == k)
            return v;
    return std::string(culture);
}

std::string vocabularyOf(std::string_view culture)
{
    for (const Culture& c : all())
        if (culture == c.name)
            return c.vocabulary;
    return "english";
}

std::string cultureForLocation(double lat, double lon, std::string* why)
{
    struct Box
    {
        const char* culture;
        const char* place;
        double s, w, n, e;
    };
    // Rough national boxes, first match wins: enough to choose words, not to draw borders.
    static const Box boxes[] = {
        {"mexican", "Mexico", 14.3, -118.5, 32.8, -86.5},
        {"usGrid", "the United States", 24.0, -125.0, 49.5, -66.0},
        {"usGrid", "Canada", 41.6, -141.0, 72.0, -52.0},
        {"spanish", "Spain", 35.9, -9.5, 43.9, 3.4},
        {"french", "France", 41.3, -5.2, 51.2, 9.6},
        {"oldEnglish", "Great Britain and Ireland", 49.8, -10.7, 60.9, 1.8},
        {"mexican", "Central and South America", -56.0, -92.0, 14.3, -34.0},
    };
    for (const Box& b : boxes)
        if (lat >= b.s && lat <= b.n && lon >= b.w && lon <= b.e)
        {
            if (why)
                *why = std::string("the data lies in ") + b.place;
            return b.culture;
        }
    if (why)
        *why = "no culture is set for the data's location; oldEnglish used";
    return "oldEnglish";
}

const std::vector<std::string>& programs()
{
    static const std::vector<std::string> p = [] {
        std::vector<std::string> out;
        for (const auto& [name, patterns] : mexicanVocab().programs)
            out.push_back(name);
        return out;
    }();
    return p;
}

std::string streetName(std::string_view culture, int cls, uint64_t h, int attempt)
{
    const Culture& c = cultureNamed(culture);
    const size_t ci = size_t(std::clamp(cls, 0, 4));
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
    {
        const std::string word = pick(c.classWords[ci], k);
        const int n = 1 + int(hash64(k) % 99);
        if (cls <= 0)
            return word + " " + std::to_string(n);
        return word + " " + char('A' + int(hash64(k ^ 7) % 26)) + std::to_string(n);
    }
    const std::string root = pick(c.roots, k), word = pick(c.classWords[ci], hash64(k ^ 0x51));
    return c.wordFirst ? word + " " + root : root + " " + word;
}

std::string districtName(std::string_view culture, uint64_t h, int attempt)
{
    const Culture& c = cultureNamed(culture);
    const uint64_t k = hash64(h + uint64_t(attempt) * 0x9E3779B97F4A7C15ull);
    if (std::string_view(c.name) == "sector")
        return "Sector " + std::to_string(1 + int(k % 40)) + char('A' + int(hash64(k) % 26));
    std::string root = pick(c.districtRoots, k);
    if (!c.districtEnds.empty())
        root += pick(c.districtEnds, hash64(k ^ 0x33));
    return c.districtPrefix + root;
}

std::string placeName(std::string_view culture, Kind kind, std::string_view program, uint64_t key, int attempt)
{
    requireCulture(culture);
    if (kind == Kind::Street)
        return streetName(culture, 2, key, attempt);
    if (kind == Kind::District)
        return districtName(culture, key, attempt);
    const Vocab& v = vocab(culture);
    const uint64_t k = hash64(key + uint64_t(attempt) * 0x9E3779B97F4A7C15ull + uint64_t(kind) * 0xD6E8FEB86659FD93ull);
    const Words* patterns = nullptr;
    if (!program.empty())
        if (auto it = v.programs.find(program); it != v.programs.end())
            patterns = &it->second;
    if (!patterns)
        if (auto it = v.kinds.find(kind); it != v.kinds.end())
            patterns = &it->second;
    if (!patterns)
        if (auto it = v.programs.find(kindProgram(kind)); it != v.programs.end())
            patterns = &it->second;
    if (!patterns)
        patterns = &v.business;
    return expand(v, pick(*patterns, k), hash64(k ^ 0x5bd1e995));
}
} // namespace lila::names
